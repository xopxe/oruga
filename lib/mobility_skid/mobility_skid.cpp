// Left / Right controller mode

#include "micro_rosso.h"

#include <rosidl_runtime_c/string_functions.h>
#include <micro_ros_utilities/string_utilities.h>

#include "mobility_skid_config.h"
#include "mobility_skid.h"

#include <geometry_msgs/msg/twist_stamped.h>
#include <sensor_msgs/msg/joint_state.h>

static geometry_msgs__msg__TwistStamped msg_cmd_vel;
static sensor_msgs__msg__JointState msg_joint_state;

static subscriber_descriptor sdescriptor_cmd_vel;

static publisher_descriptor pdescriptor_joint_state;
static double state_position[4];
static double state_velocity[4];
rosidl_runtime_c__String state_name[4];

#include "sabertooth.h"
#include <ESP32Encoder.h>
#include "fpid.h"
#include "odom_helper.h"

static Sabertooth sabertooth(128, SABERTOOTH_SERIAL);

static unsigned long set_control_time_ms = 0;

static ESP32Encoder encoder_rr_lft;
static ESP32Encoder encoder_fr_lft;
static ESP32Encoder encoder_fr_rgt;
static ESP32Encoder encoder_rr_rgt;
// static unsigned long last_encoder_us = 0UL;

static Fpid pid_lft(-126, 126, PID_LEFT_KF, PID_LEFT_KP, PID_LEFT_KI, PID_LEFT_KD);
static Fpid pid_rgt(-126, 126, PID_RIGHT_KF, PID_RIGHT_KP, PID_RIGHT_KI, PID_RIGHT_KD);

static OdomHelper odom;

// static float current_linear;
// static float current_angular;

//static float target_v_lft;
//static float target_v_rgt;
static float target_linear;
static float target_angular;

static float current_v_lft;
static float current_v_rgt;

// static float last_dt_s;

static float wheel_angular_rr_lft;
static float wheel_angular_fr_lft;
static float wheel_angular_fr_rgt;
static float wheel_angular_rr_rgt;

static int64_t enc_count_rr_lft = 0;
static int64_t enc_count_fr_lft = 0;
static int64_t enc_count_fr_rgt = 0;
static int64_t enc_count_rr_rgt = 0;

#define RCNOCHECK(fn)       \
  {                         \
    rcl_ret_t temp_rc = fn; \
    (void)temp_rc;          \
  }

class DisableInterruptsGuard
{
public:
  DisableInterruptsGuard()
  {
    noInterrupts();
  }

  ~DisableInterruptsGuard()
  {
    interrupts();
  }
};

MobilitySkid::MobilitySkid()
{
  msg_joint_state.header.frame_id = micro_ros_string_utilities_set(msg_joint_state.header.frame_id, "base_link");

  msg_joint_state.name.size = msg_joint_state.name.capacity = 4;
  msg_joint_state.name.data = state_name;
  msg_joint_state.name.data[0] = micro_ros_string_utilities_set(msg_joint_state.name.data[0], "fl_wheel_joint");
  msg_joint_state.name.data[1] = micro_ros_string_utilities_set(msg_joint_state.name.data[1], "fr_wheel_joint");
  msg_joint_state.name.data[2] = micro_ros_string_utilities_set(msg_joint_state.name.data[2], "rl_wheel_joint");
  msg_joint_state.name.data[3] = micro_ros_string_utilities_set(msg_joint_state.name.data[3], "rr_wheel_joint");

  msg_joint_state.position.size = msg_joint_state.position.capacity = 4;
  msg_joint_state.position.data = state_position;

  msg_joint_state.velocity.size = msg_joint_state.velocity.capacity = 4;
  msg_joint_state.velocity.data = state_velocity;
};

static void set_target_velocities(float linear, float angular)
{
  if (linear > MAX_SPEED)
    target_linear = MAX_SPEED;
  else if (linear < -MAX_SPEED)
    target_linear = -MAX_SPEED;
  else
    target_linear = linear;

  if (angular > MAX_TURNSPEED)
    target_angular = MAX_TURNSPEED;
  else if (angular < -MAX_TURNSPEED)
    target_angular = -MAX_TURNSPEED;
  else
     target_angular = angular;
}

static void compute_movement(float time_step)
{
  int64_t count_fr_lft;
  int64_t count_fr_rgt;
  int64_t count_rr_lft;
  int64_t count_rr_rgt;
  {
    // volatile DisableInterruptsGuard interrupt;
    count_fr_lft = encoder_fr_lft.getCount() * ENCODER_lft_MULT;
    count_fr_rgt = encoder_fr_rgt.getCount() * ENCODER_rgt_MULT;
    count_rr_lft = encoder_rr_lft.getCount() * ENCODER_lft_MULT;
    count_rr_rgt = encoder_rr_rgt.getCount() * ENCODER_rgt_MULT;
  }

  // rad/s
  float tics__to__rad_s = TICKS_TO_RAD / time_step;

  wheel_angular_fr_lft = tics__to__rad_s * (count_fr_lft - enc_count_fr_lft);
  wheel_angular_fr_rgt = tics__to__rad_s * (count_fr_rgt - enc_count_fr_rgt);
  wheel_angular_rr_lft = tics__to__rad_s * (count_rr_lft - enc_count_rr_lft);
  wheel_angular_rr_rgt = tics__to__rad_s * (count_rr_rgt - enc_count_rr_rgt);

  enc_count_fr_lft = count_fr_lft;
  enc_count_fr_rgt = count_fr_rgt;
  enc_count_rr_lft = count_rr_lft;
  enc_count_rr_rgt = count_rr_rgt;

  current_v_lft = WHEEL_RADIUS * (wheel_angular_rr_lft + wheel_angular_fr_lft) / 2; // FIXME
  current_v_rgt = WHEEL_RADIUS * (wheel_angular_rr_rgt + wheel_angular_fr_rgt) / 2;

  float current_linear = (current_v_lft + current_v_rgt) / 2;                         // m/s
  float current_angular = atan((current_v_rgt - current_v_lft) / LR_WHEELS_DISTANCE); // rad/s

  odom.update_pos(current_linear, 0.0, current_angular, time_step);

  /*
  D_print(current_linear);
  D_print(" m/s | rad/s ");
  D_println(current_angular);
  //  */
}

static void report_cb(int64_t last_call_time)
{
  msg_joint_state.velocity.data[0] = wheel_angular_fr_lft;
  msg_joint_state.velocity.data[1] = wheel_angular_fr_rgt;
  msg_joint_state.velocity.data[2] = wheel_angular_rr_lft;
  msg_joint_state.velocity.data[3] = wheel_angular_rr_rgt;

  msg_joint_state.position.data[0] = TICKS_TO_RAD * enc_count_fr_lft;
  msg_joint_state.position.data[1] = TICKS_TO_RAD * enc_count_fr_rgt;
  msg_joint_state.position.data[2] = TICKS_TO_RAD * enc_count_rr_lft;
  msg_joint_state.position.data[3] = TICKS_TO_RAD * enc_count_rr_rgt;

  // FIXME remove
  // msg_joint_state.position.data[0] += 2*PI / 10;

  micro_rosso::set_timestamp(msg_joint_state.header.stamp);
  RCNOCHECK(rcl_publish(
      &pdescriptor_joint_state.publisher,
      &msg_joint_state,
      NULL));
}

static void control_cb(int64_t last_call_time)
{
  // D_print(".");
  if (last_call_time == 0)
  {
    return;
  }

  float time_step = last_call_time / 1000000000.0; // nanosec to sec

  compute_movement(time_step);

  if (((millis() - set_control_time_ms) > STOP_TIMEOUT_MS))
  {
    // sabertooth watchdog must have stopped the motor, set as stopped and skip
    MobilitySkid::stop();
    return;
  }

  // reverse kinematics
  float v_diff = LR_WHEELS_OFFSET * target_angular;
  float target_v_lft = target_linear - v_diff;
  float target_v_rgt = target_linear + v_diff;

  float power_lft = pid_lft.compute(target_v_lft, current_v_lft, last_call_time);
  float power_rgt = pid_rgt.compute(target_v_rgt, current_v_rgt, last_call_time);

  /*
  D_print(power_lft);
  D_print(" ! ");
  D_println(power_rgt);
  // */

  sabertooth.motor(1, MOTOR_OUT_lft_MULT * (int8_t)power_lft);
  sabertooth.motor(2, MOTOR_OUT_rgt_MULT * (int8_t)power_rgt);
}

void MobilitySkid::stop()
{
  set_target_velocities(0.0, 0.0);
  pid_lft.reset_errors(0);
  pid_rgt.reset_errors(0);
  sabertooth.stop();
}

// TODO
void MobilitySkid::set_motor_enable(boolean enable)
{
}

static void cmd_vel_cb(const void *cmd_vel)
{
  set_control_time_ms = millis();

  // TODO verify msg_cmd_vel.header.stamp

  set_target_velocities(
      msg_cmd_vel.twist.linear.x,
      msg_cmd_vel.twist.angular.z);

  /*
  D_print("cmd_vel: ");
  D_print(msg_cmd_vel.linear.x);
  D_print(" ");
  D_println(msg_cmd_vel.angular.z);
  */
}

bool MobilitySkid::setup()
{
  if (!odom.setup("/odom", "odom", "base_footprint"))
  {
    D_println("failure initializing odom_helper.");
    return false;
  }

  /* Initialize the motor driver */
  D_print("setup: sabertooth... ");
  SABERTOOTH_SERIAL.begin(9600, SERIAL_8N1, -1, SABERTOOTH_TX_PIN);
  sabertooth.drive(0);
  sabertooth.turn(0);
  sabertooth.setTimeout(STOP_TIMEOUT_MS);
  D_println("done.");

  // ESP32Encoder::useInternalWeakPullResistors = puType::down;
  //  Enable the weak pull up resistors
  ESP32Encoder::useInternalWeakPullResistors = puType::up;

  // attachSingleEdge
  // attachFullQuad
  D_print("setup: encoders... ");
#if defined(USE_FULL_QUADRATURE)
  encoder_rr_lft.attachFullQuad(ENCODER_rr_lft_PIN_A, ENCODER_rr_lft_PIN_B);
  encoder_fr_lft.attachFullQuad(ENCODER_fr_lft_PIN_A, ENCODER_fr_lft_PIN_B);
  encoder_fr_rgt.attachFullQuad(ENCODER_fr_rgt_PIN_A, ENCODER_fr_rgt_PIN_B);
  encoder_rr_rgt.attachFullQuad(ENCODER_rr_rgt_PIN_A, ENCODER_rr_rgt_PIN_B);
#elif
  encoder_rr_lft.attachSingleEdge(ENCODER_rr_lft_PIN_A, ENCODER_rr_lft_PIN_B);
  encoder_fr_lft.attachSingleEdge(ENCODER_fr_lft_PIN_A, ENCODER_fr_lft_PIN_B);
  encoder_fr_rgt.attachSingleEdge(ENCODER_fr_rgt_PIN_A, ENCODER_fr_rgt_PIN_B);
  encoder_rr_rgt.attachSingleEdge(ENCODER_rr_rgt_PIN_A, ENCODER_rr_rgt_PIN_B);
#endif

  encoder_rr_lft.clearCount();
  encoder_fr_lft.clearCount();
  encoder_fr_rgt.clearCount();
  encoder_rr_rgt.clearCount();
  // last_encoder_us = micros();
  D_println("done.");

  /*
  D_print("MAX_WHEEL_ANGULAR ");D_println((float)MAX_WHEEL_ANGULAR);
  D_print("MAX_SPEED ");D_println((float)MAX_SPEED);
  D_print("MAX_TURNSPEED ");D_println((float)MAX_TURNSPEED);
  D_print("PID_LINEAL_KF ");D_println(PID_LINEAL_KF);
  D_print("PID_TURN_KF ");D_println((float)PID_TURN_KF);
  */

  D_print("setup: mobility_skid...");
  sdescriptor_cmd_vel.qos = QOS_BEST_EFFORT;
  sdescriptor_cmd_vel.type_support =
      (rosidl_message_type_support_t *)ROSIDL_GET_MSG_TYPE_SUPPORT(geometry_msgs, msg, TwistStamped);
  sdescriptor_cmd_vel.topic_name = SKID_TOPIC_CMD_VEL;
  sdescriptor_cmd_vel.msg = &msg_cmd_vel;
  sdescriptor_cmd_vel.callback = &cmd_vel_cb;
  micro_rosso::subscribers.push_back(&sdescriptor_cmd_vel);

  pdescriptor_joint_state.qos = QOS_DEFAULT;
  pdescriptor_joint_state.type_support =
      (rosidl_message_type_support_t *)ROSIDL_GET_MSG_TYPE_SUPPORT(
          sensor_msgs, msg, JointState);
  pdescriptor_joint_state.topic_name = "/joint_states";
  micro_rosso::publishers.push_back(&pdescriptor_joint_state);

  micro_rosso::timer_control.callbacks.push_back(&control_cb);
  micro_rosso::timer_report.callbacks.push_back(&report_cb);

  D_println("done.");
  return true;
}
