
// try this, e.g. with roslaunch stdr_launchers server_with_map_and_gui_plus_robot.launch
// or:  roslaunch cwru_376_launchers stdr_glennan_2.launch
// watch resulting velocity commands with: rqt_plot /robot0/cmd_vel/linear/x (or jinx/cmd_vel...)

//intent of this program: modulate the velocity command to comply with a speed limit, v_max,
// acceleration limits, +/-a_max, and come to a halt gracefully at the end of
// an intended line segment

// notes on quaternions:
/*
From:
http://www.euclideanspace.com/maths/geometry/rotations/conversions/angleToQuaternion/

qx = ax * sin(angle/2)
qy = ay * sin(angle/2)
qz = az * sin(angle/2)
qw = cos(angle/2)


so, quaternion in 2-D plane (x,y,theta):
ax=0, ay=0, az = 1.0

qx = 0;
qy = 0;
qz = sin(angle/2)
qw = cos(angle/2)

therefore, theta = 2*atan2(qz,qw)
*/
#include <vel_scheduler.h>
#define PI 3.14159265

// set some dynamic limits
const double v_max = 0.6; //1m/sec is a slow walk
const double v_min = 0.1; // if command velocity too low, robot won't move
const double a_max = 0.3; //1m/sec^2 is 0.1 g's
//const double a_max_decel = 0.1; // TEST
const double omega_max = 0.6; //1 rad/sec-> about 6 seconds to rotate 1 full rev
const double alpha_max = 0.3; //0.5 rad/sec^2-> takes 2 sec to get from rest to full omega
const double DT = 0.050; // choose an update rate of 20Hz; go faster with actual hardware

//Variables to store the motorsEnabled information
bool motorsEnabled;
bool motorsEnabled_ = true; //global variable to store motorsEnabled status

string check;

//Variables to store the lidar alarm information
bool lidar_alarm;
bool lidar_alarm_ = false; //global variable to store lidar status
string lidar_check;

//Soft_stop variables
bool soft_stop_ = false;  //in future add callback message


//arrays that hold the segment and turn information for plotting the course
double segments [] = {2, 0.0, 1.0, 0.0, 2.0, 0.0}; //variable to store movement segments
double turns [] = {0.0, PI / 2, 0.0, PI / 2, 0.0, PI / 2}; //variable to store turn segments
int counter = 1; //counts through segment and turn arrays


//starting at first values in array
double segment_length = segments [0];
double angle_rotation = turns [0];

// globals for communication w/ callbacks:
double odom_vel_ = 0.0; // measured/published system speed
double odom_omega_ = 0.0; // measured/published system yaw rate (spin)
double odom_x_ = 0.0;
double odom_y_ = 0.0;
double odom_phi_ = 0.0;
double last_odom_phi_ = 0.0;
int rot_count_ = 0;
double dt_odom_ = 0.0;
ros::Time t_last_callback_;
double dt_callback_ = 0.0;

// receive the pose and velocity estimates from the simulator (or the physical robot)
// copy the relevant values to global variables, for use by "main"
// Note: stdr updates odom only at 10Hz; Jinx is 50Hz (?)
void odomCallback(const nav_msgs::Odometry &odom_rcvd)
{
    dt_callback_ = (ros::Time::now() - t_last_callback_).toSec();       //here's a trick to compute the delta-time between successive callbacks:
    t_last_callback_ = ros::Time::now();        // let's remember the current time, and use it next iteration


    // on start-up, and with occasional hiccups, this delta-time can be unexpectedly large
    if (dt_callback_ > 0.15)
    {
        dt_callback_ = 0.1; // can choose to clamp a max value on this, if dt_callback is used for computations elsewhere
        ROS_WARN("large dt; dt = %lf", dt_callback_);   // let's complain whenever this happens

    }

    // copy some of the components of the received message into global vars, for use by "main()"
    // we care about speed and spin, as well as position estimates x,y and heading
    odom_vel_ = odom_rcvd.twist.twist.linear.x;
    odom_omega_ = odom_rcvd.twist.twist.angular.z;
    odom_x_ = odom_rcvd.pose.pose.position.x;
    odom_y_ = odom_rcvd.pose.pose.position.y;
    //odom publishes orientation as a quaternion.  Convert this to a simple heading
    // see notes above for conversion for simple planar motion
    double quat_z = odom_rcvd.pose.pose.orientation.z;
    double quat_w = odom_rcvd.pose.pose.orientation.w;
    // cheap conversion from quaternion to heading for planar motion
    double raw_odom_phi_ = 2.0 * atan2(quat_z, quat_w);
    ROS_WARN("raw_odom_phi is %lf", raw_odom_phi_);
    if ((last_odom_phi_ - raw_odom_phi_) > (1.5*PI))
    {
        rot_count_++;
    }
    else if ((last_odom_phi_ - raw_odom_phi_) < (-1.5*PI))
    {
        rot_count_--;
    }
    last_odom_phi_ = raw_odom_phi_;

    odom_phi_ = (rot_count_ * 2 * PI) + raw_odom_phi_;
    // the output below could get annoying; may comment this out, but useful initially for debugging
    ROS_INFO("odom CB: x = %f, y= %f, phi = %f, v = %f, omega = %f", odom_x_, odom_y_, odom_phi_, odom_vel_, odom_omega_);
}

//store motorsEnabled information in global variable
void motorsEnabledCallback(const std_msgs::Bool::ConstPtr &motorsEnabled)
{
    if (motorsEnabled->data == true)
    {
        check = "motorsEnabled_on";  // means motors are ENABLED
        motorsEnabled_ = true;
    }
    else if (motorsEnabled->data == false)
    {
        check = "motorsEnabled_off";  // means motors are DISABLED
        motorsEnabled_ = false;
    }


    ROS_INFO("%s", check.c_str());
}

//store lidar information in global variable
void lidarCallback(const std_msgs::Bool &lidar_alarm)
{
    if (lidar_alarm.data == true)
    {
        lidar_check = "lidar_alarm_on";
        lidar_alarm_ = true;
    }

    else if (lidar_alarm.data == false)
    {
        lidar_check = "lidar_alarm_off";
        lidar_alarm_ = false;
    }

    ROS_INFO("%s", lidar_check.c_str());
}

//cycle through the array of distance moves
int segmentCycle(double segment [], double turn [], int i)
{
    segment_length = segment [i];
    angle_rotation = turn [i];
    ROS_INFO("Segment length: %f, Rotation Angle: %f", segment_length, angle_rotation);
}

double determineScheduledVel(double dist_to_go, double dist_decel)
{
    // at goal, or overshot; stop!
    if (dist_to_go <= 0.0)
    {
        return 0.0;
    }

    //possibly should be braking to a halt
    // dist = 0.5*a*t_halt^2; so t_halt = sqrt(2*dist/a);   v = a*t_halt
    // so v = a*sqrt(2*dist/a) = sqrt(2*dist*a)
    else if (dist_to_go <= dist_decel)
    {
        double scheduled_vel = .5 * sqrt(2 * dist_to_go * a_max);
        ROS_WARN("Braking Zone: First V_Sched = %f", scheduled_vel);
        return scheduled_vel;
    }

    // not ready to decel, so target vel is v_max, either accel to it or hold it
    else
    {
        return v_max;
    }

}

double determineScheduledOmega(double angle_to_turn, double rot_decel, double rot_direction)
{
    //Use the amount turned to decide on the rate of rotation
    if (angle_to_turn <= 0.01 && angle_to_turn >= -0.01)
    {
        //if we have reached the angle we were trying to turn to
        return 0.0;
    }

    else if (fabs(angle_to_turn) <= rot_decel)
    {
        double scheduled_omega = rot_direction * sqrt(2 * fabs(angle_to_turn) * alpha_max); //should be slowing down our rotation if we are past the angle necessary to decel
        ROS_WARN("Breaking zone: First Omega_Sched = %f", scheduled_omega);
        return scheduled_omega;
    }

    //not a point of decel therefore try and run at max turn, or accelerate the turn to max turn
    else
    {
        return rot_direction * omega_max;
    }
}

double determineCmdVel(double scheduled_vel)
{
	//how does the current velocity compare to the scheduled vel?
	// maybe we halted, e.g. due to estop or obstacle;
	if (odom_vel_ < scheduled_vel)
	{
	    // may need to ramp up to v_max; do so within accel limits
	    double v_test = odom_vel_ + (a_max * dt_callback_); // if callbacks are slow, this could be abrupt
	    // operator:  c = (a>b) ? a : b;
	    double new_cmd_vel = (v_test < scheduled_vel) ? v_test : scheduled_vel; //choose lesser of two options
	    ROS_INFO("Ramping up velocity: New Cmd Vel: %f, Sched Vel: %f", new_cmd_vel, scheduled_vel);
	    return new_cmd_vel;
	}

	//travelling too fast--this could be trouble
	else if (odom_vel_ > scheduled_vel)
	{
	    // ramp down to the scheduled velocity.  However, scheduled velocity might already be ramping down at a_max.
	    // need to catch up, so ramp down even faster than a_max.  Try 1.2*a_max.
	    double v_test = odom_vel_ - (1.2 * a_max * dt_callback_); //moving too fast--try decelerating faster than nominal a_max

	    double new_cmd_vel = (v_test > scheduled_vel) ? v_test : scheduled_vel; // choose larger of two options...don't overshoot scheduled_vel

	    ROS_INFO("Slowing Down velocity: New Cmd Vel: %f; Sched Vel: %f", new_cmd_vel, scheduled_vel); //debug/analysis output; can comment this out
	    return new_cmd_vel;
	}

	else
	{
	    return scheduled_vel; //silly third case: this is already true, if here.  Issue the scheduled velocity
	}
}

double determineCmdOmega(double scheduled_omega, double rot_direction)
{
	//compare the current turning speed to the scheduled turning speed
	if (fabs(odom_omega_) < fabs(scheduled_omega))
	{
	    //for some reason the turning speed is less than schedule
	    double omega_test = odom_omega_ + (rot_direction * alpha_max * dt_callback_);
	    //create two options for turning
	    double new_cmd_omega = (fabs(omega_test) < fabs(scheduled_omega)) ? omega_test : scheduled_omega; // choose lesser of the two turn speeds
	    //done in order to prevent overshooting the scheduled_omega
	    ROS_INFO("Ramping Up rotation: New cmd omega: %f, Sched Omega: %f", new_cmd_omega, scheduled_omega); //debugging information
	    return new_cmd_omega;
	}

	// for some reason we are traveling too fast
	else if (fabs(odom_omega_) > fabs(scheduled_omega))
	{
	    //lets ramp down at 1.2*alpha_max in case we are already trying to decel
	    double omega_test = (rot_direction * fabs(odom_omega_)) - (1.2 * alpha_max * dt_callback_); //turning too fast, slow down faster than normal
	    double new_cmd_omega = (fabs(omega_test) > fabs(scheduled_omega)) ? omega_test : scheduled_omega; //choose the larger of the two options, as to not overshoot scheduled_omega

	    ROS_INFO("Slowing Down rotation: New cmd omega: %f; Sched omega: %f", new_cmd_omega, scheduled_omega); //debug/analysis output; can comment this out
		return new_cmd_omega;
	}

	else
	{
	    return scheduled_omega;//apply the scheduled turn speed if everything else is fine
	}
}

void checkAlarms(geometry_msgs::Twist &cmd_vel, double rot_direction)
{
	//begin decel to stop if lidar alarm or soft stop is on
	if (lidar_alarm_ == true || soft_stop_ == true)
	{

	    ROS_WARN("LIDAR OR SOFT STOP");

	    if (odom_vel_ >= .01)
	    {
	        cmd_vel.linear.x = odom_vel_ - (a_max * dt_callback_); //decel
	    }

	    else cmd_vel.linear.x = 0;  //velocity 0 if slow enough


	    if (fabs(odom_omega_) >= .01)
	    {
	        cmd_vel.angular.z = (rot_direction * fabs(odom_omega_)) - (alpha_max * dt_callback_); //decel
	    }

	    else cmd_vel.angular.z = 0; //omega 0 if slow enough

	}

	if (motorsEnabled_ == false)
	{
	    ROS_WARN("ESTOP ACTIVATED");
	    cmd_vel.linear.x = 0.0;
	    cmd_vel.angular.z = 0.0;
	}
}

int main(int argc, char **argv)
{
    ros::init(argc, argv, "vel_scheduler"); // name of this node will be "minimal_publisher1"

    ros::NodeHandle nh; // get a ros nodehandle; standard yadda-yadda

    //subscribe to motors_enabled rosmsg
    ros::Subscriber submotorsEnabled = nh.subscribe("/motors_enabled", 1, motorsEnabledCallback);

    //subscribe to lidar_alarm rosmsg
    ros::Subscriber sublidar = nh.subscribe("/lidar_alarm", 1, lidarCallback);

    //create a publisher object that can talk to ROS and issue twist messages on named topic;
    // note: this is customized for stdr robot; would need to change the topic to talk to jinx, etc.
    ros::Publisher vel_cmd_publisher = nh.advertise<geometry_msgs::Twist>("/cmd_vel", 1);
    ros::Subscriber sub = nh.subscribe("/odom", 1, odomCallback);
    ros::Rate rtimer(1 / DT); // frequency corresponding to chosen sample period DT; the main loop will run this fast

    double segment_length_done = 0.0; // need to compute actual distance travelled within the current segment
    double start_x = 0.0; // fill these in with actual values once odom message is received
    double start_y = 0.0; // subsequent segment start coordinates should be specified relative to end of previous segment
    double start_phi = 0.0; //starting angle of the robot

    double scheduled_vel = 0.0; //desired vel, assuming all is per plan
    double scheduled_omega = 0.0; //desired omega, assuming all is per plan
    double new_cmd_vel = 0.1; // value of speed to be commanded; update each iteration
    double new_cmd_omega = 0.1; // update spin rate command as well

    geometry_msgs::Twist cmd_vel; //create a variable of type "Twist" to publish speed/spin commands

    cmd_vel.linear.x = 0.0; // initialize these values to zero
    cmd_vel.linear.y = 0.0;
    cmd_vel.linear.z = 0.0;
    cmd_vel.angular.x = 0.0;
    cmd_vel.angular.y = 0.0;
    cmd_vel.angular.z = 0.0;

    // let's wait for odom callback to start getting good values
    odom_omega_ = 1000000;

    ROS_INFO("waiting for valid odom callback...");

    // initialize reference for computed update rate of callback
    t_last_callback_ = ros::Time::now();
    ROS_WARN("Successfully passed the time now of death");

    while (odom_omega_ > 1000)
    {
        rtimer.sleep();
        ros::spinOnce();
    }

    ROS_INFO("received odom message; proceeding %f", odom_omega_);

    //starting points for segment or rotation regimes
    start_x = odom_x_;
    start_y = odom_y_;
    start_phi = odom_phi_;

    ROS_INFO("start pose: x %f, y= %f, phi = %f", start_x, start_y, start_phi);

    // compute some properties of trapezoidal velocity profile plan:
    double T_accel = v_max / a_max; //...assumes start from rest
    double T_decel = v_max / a_max; //(for same decel as accel); assumes brake to full halt
    double dist_accel = 0.5 * a_max * (T_accel * T_accel); //distance rqd to ramp up to full speed
    double dist_decel = 0.5 * a_max * (T_decel * T_decel);; //same as ramp-up distance
    double dist_const_v = segment_length - dist_accel - dist_decel; //if this is <0, never get to full spd
    double T_const_v = dist_const_v / v_max; //will be <0 if don't get to full speed
    double T_segment_tot = T_accel + T_decel + T_const_v; // expected duration of this move

    //compute some properties of trapezoidal rotation profile plan
    double T_accel_alpha = omega_max / alpha_max;
    double T_decel_alpha = omega_max / alpha_max;
    double rot_accel = 0.5 * alpha_max * (T_accel_alpha * T_accel_alpha);
    double rot_decel = 0.5 * alpha_max * (T_decel_alpha * T_decel_alpha);
    double rot_const_omega = angle_rotation - rot_accel - rot_decel;
    double T_const_omega = rot_const_omega / omega_max;
    double T_rot_total = T_accel_alpha + T_decel_alpha + T_const_omega;

    //dist_decel*= 2.0; // TEST TEST TEST
    while (ros::ok()) // do work here in infinite loop (desired for this example), but terminate if detect ROS has faulted (or ctl-C)
    {
        ros::spinOnce(); // allow callbacks to populate fresh data
        // compute distance travelled so far:
        double delta_x = odom_x_ - start_x;
        double delta_y = odom_y_ - start_y;
        segment_length_done = sqrt(delta_x * delta_x + delta_y * delta_y);

        //compute angle rotated thus far
        double delta_phi = odom_phi_ - start_phi;

        //distance left to go, and angle left to turn
        double dist_to_go = segment_length - segment_length_done;
        double angle_to_turn = angle_rotation - delta_phi;
        double rot_direction = angle_to_turn / fabs(angle_to_turn);

        ROS_INFO("dist travelled: %f, dist to travel: %f, angle turned: %f, angle to turn: %f", segment_length_done, dist_to_go, delta_phi, angle_to_turn);

        //use segment_length_done to decide what vel should be, as per plan

        scheduled_vel = determineScheduledVel(dist_to_go, dist_decel);
        scheduled_omega = determineScheduledOmega(angle_to_turn, rot_decel, rot_direction);


        cmd_vel.linear.x = determineCmdVel(scheduled_vel);
        cmd_vel.angular.z = determineCmdOmega(scheduled_omega, rot_direction);

        ROS_INFO("cmd vel: %f", cmd_vel.linear.x); // debug output
        ROS_INFO("cmd omega: %f", cmd_vel.angular.z); // debug output

        checkAlarms(cmd_vel, rot_direction);

        //uh-oh...went too far already! or the estop is true!
        if (dist_to_go <= 0.0)
        {
            cmd_vel.linear.x = 0.0;  //command vel=0
        }

        //we overshot, just stop
        if ((angle_to_turn <= 0.01 && angle_to_turn >= -0.01))
        {
            cmd_vel.angular.z = 0.0;
        }

        vel_cmd_publisher.publish(cmd_vel); // publish the command to robot0/cmd_vel
        rtimer.sleep(); // sleep for remainder of timed iteration

        ROS_INFO("Final new_cmd_vel: %f", cmd_vel.linear.x);

        //after finishing current move, cycle array, reinitialize starting values for next iteration
        if (dist_to_go <= 0.0 && (angle_to_turn <= 0.01 && angle_to_turn >= -0.01))
        {
            segmentCycle(segments, turns, counter);
            counter ++;
            start_x = odom_x_;
            start_y = odom_y_;
            start_phi = odom_phi_;
        }
    }
    ROS_INFO("completed move distance");
}