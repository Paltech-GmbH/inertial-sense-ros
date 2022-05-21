#include "inertial_sense_ros.h"
#include <chrono>
#include <stddef.h>
#include <unistd.h>
#include <tf/tf.h>
#include <ros/console.h>
#include <ISPose.h>
#include <ISEarth.h>
#include <tf2/LinearMath/Quaternion.h>
#include "ISMatrix.h"
#include "ISEarth.h"

InertialSenseROS::InertialSenseROS() : nh_(), nh_private_("~"), initialized_(false), rtk_connectivity_watchdog_timer_()
{
    connect();
    set_navigation_dt_ms();

    /// Start Up ROS service servers
    refLLA_set_current_srv_ = nh_.advertiseService("set_refLLA_current", &InertialSenseROS::set_current_position_as_refLLA, this);
    refLLA_set_value_srv_ = nh_.advertiseService("set_refLLA_value", &InertialSenseROS::set_refLLA_to_value, this);
    mag_cal_srv_ = nh_.advertiseService("single_axis_mag_cal", &InertialSenseROS::perform_mag_cal_srv_callback, this);
    multi_mag_cal_srv_ = nh_.advertiseService("multi_axis_mag_cal", &InertialSenseROS::perform_multi_mag_cal_srv_callback, this);
    firmware_update_srv_ = nh_.advertiseService("firmware_update", &InertialSenseROS::update_firmware_srv_callback, this);

    configure_flash_parameters();
    configure_data_streams();
    configure_rtk();

    nh_private_.param<bool>("enable_log", log_enabled_, false);
    if (log_enabled_)
    {
        start_log(); // start log should always happen last, does not all stop all message streams.
    }

    //  configure_ascii_output(); //does not work right now

    initialized_ = true;
}

void InertialSenseROS::configure_data_streams()
{
    
    IS_.StopBroadcasts(true);
    SET_CALLBACK(DID_STROBE_IN_TIME, strobe_in_time_t, strobe_in_time_callback, 1); // we always want the strobe
    nh_private_.param<bool>("stream_DID_INS_1", DID_INS_1_.enabled, false);
    nh_private_.param<bool>("stream_DID_INS_2", DID_INS_2_.enabled, false);
    nh_private_.param<bool>("stream_DID_INS_4", DID_INS_4_.enabled, false);
    nh_private_.param<bool>("stream_odom_ins_ned", odom_ins_ned_.enabled, true);
    nh_private_.param<bool>("stream_odom_ins_enu", odom_ins_enu_.enabled, false);
    nh_private_.param<bool>("stream_odom_ins_ecef", odom_ins_ecef_.enabled, false);
    nh_private_.param<bool>("stream_covariance_data", covariance_enabled_, false);
    nh_private_.param<bool>("stream_INL2_states", INL2_states_.enabled, false);
    nh_private_.param<bool>("stream_IMU", IMU_.enabled, true);
    nh_private_.param<bool>("stream_GPS", GPS_.enabled, true);
    nh_private_.param<bool>("stream_GPS_raw", GPS_obs_.enabled, false);
    nh_private_.param<bool>("stream_GPS_raw", GPS_eph_.enabled, false);
    nh_private_.param<bool>("stream_GPS_info", GPS_info_.enabled, false);
    nh_private_.param<bool>("stream_NavSatFix", NavSatFix_.enabled, false);
    nh_private_.param<bool>("stream_mag", mag_.enabled, false);
    nh_private_.param<bool>("stream_baro", baro_.enabled, false);
    nh_private_.param<bool>("stream_preint_IMU", preint_IMU_.enabled, false);
    nh_private_.param<bool>("stream_diagnostics", diagnostics_.enabled, true);
    nh_private_.param<bool>("publishTf", publishTf, true);

    SET_CALLBACK(DID_FLASH_CONFIG, nvm_flash_cfg_t, flash_config_callback, 1000);

    if (DID_INS_1_.enabled)
    {
        DID_INS_1_.pub = nh_.advertise<inertial_sense_ros::DID_INS1>("DID_INS_1", 1);
        SET_CALLBACK(DID_INS_1, ins_1_t, INS1_callback, 1);
    }
    if (DID_INS_2_.enabled)
    {
        DID_INS_2_.pub = nh_.advertise<inertial_sense_ros::DID_INS2>("DID_INS_2", 1);
        SET_CALLBACK(DID_INS_2, ins_2_t, INS2_callback, 1);
    }
    if (DID_INS_4_.enabled)
    {
        DID_INS_4_.pub = nh_.advertise<inertial_sense_ros::DID_INS4>("DID_INS_4", 1);
        SET_CALLBACK(DID_INS_4, ins_4_t, INS4_callback, 1);
    }
    if (odom_ins_ned_.enabled)
    {
        odom_ins_ned_.pub = nh_.advertise<nav_msgs::Odometry>("odom_ins_ned", 1);
        SET_CALLBACK(DID_INS_4, ins_4_t, INS4_callback, 1);                                                   // Need NED
        if (covariance_enabled_)
            SET_CALLBACK(DID_ROS_COVARIANCE_POSE_TWIST, ros_covariance_pose_twist_t, INS_covariance_callback, 200); // Need Covariance data
        SET_CALLBACK(DID_PREINTEGRATED_IMU, preintegrated_imu_t, preint_IMU_callback, 1);                     // Need angular rate data from IMU
        IMU_.enabled = true;
        // Create Identity Matrix
        //
        for (int row = 0; row < 6; row++)
        {
            for (int col = 0; col < 6; col++)
            {
                if (row == col)
                {
                    ned_odom_msg.pose.covariance[row * 6 + col] = 1;
                    ned_odom_msg.twist.covariance[row * 6 + col] = 1;
                }
                else
                {
                    ned_odom_msg.pose.covariance[row * 6 + col] = 0;
                    ned_odom_msg.twist.covariance[row * 6 + col] = 0;
                }
            }
        }
    }

    if (odom_ins_ecef_.enabled)
    {
        odom_ins_ecef_.pub = nh_.advertise<nav_msgs::Odometry>("odom_ins_ecef", 1);
        SET_CALLBACK(DID_INS_4, ins_4_t, INS4_callback, 1);                                                   // Need quaternion and ecef
        if (covariance_enabled_)
            SET_CALLBACK(DID_ROS_COVARIANCE_POSE_TWIST, ros_covariance_pose_twist_t, INS_covariance_callback, 200); // Need Covariance data
        SET_CALLBACK(DID_PREINTEGRATED_IMU, preintegrated_imu_t, preint_IMU_callback, 1);                                              // Need angular rate data from IMU
        IMU_.enabled = true;
        // Create Identity Matrix
        //
        for (int row = 0; row < 6; row++)
        {
            for (int col = 0; col < 6; col++)
            {
                if (row == col)
                {
                    ecef_odom_msg.pose.covariance[row * 6 + col] = 1;
                    ecef_odom_msg.twist.covariance[row * 6 + col] = 1;
                }
                else
                {
                    ecef_odom_msg.pose.covariance[row * 6 + col] = 0;
                    ecef_odom_msg.twist.covariance[row * 6 + col] = 0;
                }
            }
        }
    }
    if (odom_ins_enu_.enabled)
    {
        odom_ins_enu_.pub = nh_.advertise<nav_msgs::Odometry>("odom_ins_enu", 1);
        SET_CALLBACK(DID_INS_4, ins_4_t, INS4_callback, 1);                                                   // Need ENU
        if (covariance_enabled_)
            SET_CALLBACK(DID_ROS_COVARIANCE_POSE_TWIST, ros_covariance_pose_twist_t, INS_covariance_callback, 200); // Need Covariance data
        SET_CALLBACK(DID_PREINTEGRATED_IMU, preintegrated_imu_t, preint_IMU_callback, 1);                                              // Need angular rate data from IMU
        IMU_.enabled = true;
        // Create Identity Matrix
        //
        for (int row = 0; row < 6; row++)
        {
            for (int col = 0; col < 6; col++)
            {
                if (row == col)
                {
                    enu_odom_msg.pose.covariance[row * 6 + col] = 1;
                    enu_odom_msg.twist.covariance[row * 6 + col] = 1;
                }
                else
                {
                    enu_odom_msg.pose.covariance[row * 6 + col] = 0;
                    enu_odom_msg.twist.covariance[row * 6 + col] = 0;
                }
            }
        }
    }

    if (NavSatFix_.enabled)
    {
        NavSatFix_.pub = nh_.advertise<sensor_msgs::NavSatFix>("NavSatFix", 1);

        // Satellite system constellation used in GNSS solution.  (see eGnssSatSigConst) 0x0003=GPS, 0x000C=QZSS, 0x0030=Galileo, 0x00C0=Beidou, 0x0300=GLONASS, 0x1000=SBAS
        uint16_t gnssSatSigConst = IS_.GetFlashConfig().gnssSatSigConst;

        if (gnssSatSigConst & GNSS_SAT_SIG_CONST_GPS)
        {
            NavSatFix_msg.status.service |= NavSatFixService::SERVICE_GPS;
        }
        if (gnssSatSigConst & GNSS_SAT_SIG_CONST_GLO)
        {
            NavSatFix_msg.status.service |= NavSatFixService::SERVICE_GLONASS;
        }
        if (gnssSatSigConst & GNSS_SAT_SIG_CONST_BDS)
        {
            NavSatFix_msg.status.service |= NavSatFixService::SERVICE_COMPASS; // includes BeiDou.
        }
        if (gnssSatSigConst & GNSS_SAT_SIG_CONST_GAL)
        {
            NavSatFix_msg.status.service |= NavSatFixService::SERVICE_GALILEO;
        }

        // DID_GPS1_POS and DID_GPS1_VEL are always streamed for fix status. See below
    }

    if (INL2_states_.enabled)
    {
        INL2_states_.pub = nh_.advertise<inertial_sense_ros::INL2States>("inl2_states", 1);
        SET_CALLBACK(DID_INL2_STATES, inl2_states_t, INL2_states_callback, 1);
    }

    if (GPS_.enabled)
    {
        GPS_.pub = nh_.advertise<inertial_sense_ros::GPS>("gps", 1);
    }
    // Set up the GPS ROS stream - we always need GPS information for time sync, just don't always need to publish it
    SET_CALLBACK(DID_GPS1_POS, gps_pos_t, GPS_pos_callback, 1); // we always need GPS for Fix status
    SET_CALLBACK(DID_GPS1_VEL, gps_vel_t, GPS_vel_callback, 1); // we always need GPS for Fix status

    if (GPS_obs_.enabled)
    {
        GPS_obs_.pub = nh_.advertise<inertial_sense_ros::GNSSObsVec>("gps/obs", 50);
        GPS_eph_.pub = nh_.advertise<inertial_sense_ros::GNSSEphemeris>("gps/eph", 50);
        GPS_geph_.pub = nh_.advertise<inertial_sense_ros::GlonassEphemeris>("gps/geph", 50);
        SET_CALLBACK(DID_GPS1_RAW, gps_raw_t, GPS_raw_callback, 1);
        SET_CALLBACK(DID_GPS_BASE_RAW, gps_raw_t, GPS_raw_callback, 1);
        SET_CALLBACK(DID_GPS2_RAW, gps_raw_t, GPS_raw_callback, 1);
        obs_bundle_timer_ = nh_.createTimer(ros::Duration(0.001), InertialSenseROS::GPS_obs_bundle_timer_callback, this);
    }

    // Set up the GPS info ROS stream
    if (GPS_info_.enabled)
    {
        GPS_info_.pub = nh_.advertise<inertial_sense_ros::GPSInfo>("gps/info", 1);
        SET_CALLBACK(DID_GPS1_SAT, gps_sat_t, GPS_info_callback, 1);
    }

    // Set up the magnetometer ROS stream
    if (mag_.enabled)
    {
        mag_.pub = nh_.advertise<sensor_msgs::MagneticField>("mag", 1);
        SET_CALLBACK(DID_MAGNETOMETER, magnetometer_t, mag_callback, 1);
    }

    // Set up the barometer ROS stream
    if (baro_.enabled)
    {
        baro_.pub = nh_.advertise<sensor_msgs::FluidPressure>("baro", 1);
        SET_CALLBACK(DID_BAROMETER, barometer_t, baro_callback, 1);
    }

    // Set up the preintegrated IMU (coning and sculling integral) ROS stream
    if (preint_IMU_.enabled)
    {
        preint_IMU_.pub = nh_.advertise<inertial_sense_ros::PreIntIMU>("preint_imu", 1);
        SET_CALLBACK(DID_PREINTEGRATED_IMU, preintegrated_imu_t, preint_IMU_callback, 1);
    }
    if(IMU_.enabled)
    {
        IMU_.pub = nh_.advertise<sensor_msgs::Imu>("imu", 1);
        SET_CALLBACK(DID_PREINTEGRATED_IMU, preintegrated_imu_t, preint_IMU_callback, 1);
    }

    // Set up ROS dianostics for rqt_robot_monitor
    if (diagnostics_.enabled)
    {
        diagnostics_.pub = nh_.advertise<diagnostic_msgs::DiagnosticArray>("diagnostics", 1);
        diagnostics_timer_ = nh_.createTimer(ros::Duration(0.5), &InertialSenseROS::diagnostics_callback, this); // 2 Hz
    }
}

void InertialSenseROS::start_log()
{
    std::string filename = getenv("HOME");
    filename += "/Documents/Inertial_Sense/Logs/" + cISLogger::CreateCurrentTimestamp();
    ROS_INFO_STREAM("Creating log in " << filename << " folder");
    IS_.SetLoggerEnabled(true, filename, cISLogger::LOGTYPE_DAT, RMC_PRESET_PPD_GROUND_VEHICLE);
}

void InertialSenseROS::configure_ascii_output()
{
    //  int NMEA_rate = nh_private_.param<int>("NMEA_rate", 0);
    //  int NMEA_message_configuration = nh_private_.param<int>("NMEA_configuration", 0x00);
    //  int NMEA_message_ports = nh_private_.param<int>("NMEA_ports", 0x00);
    //  ascii_msgs_t msgs = {};
    //  msgs.options = (NMEA_message_ports & NMEA_SER0) ? RMC_OPTIONS_PORT_SER0 : 0; // output on serial 0
    //  msgs.options |= (NMEA_message_ports & NMEA_SER1) ? RMC_OPTIONS_PORT_SER1 : 0; // output on serial 1
    //  msgs.gpgga = (NMEA_message_configuration & NMEA_GPGGA) ? NMEA_rate : 0;
    //  msgs.gpgll = (NMEA_message_configuration & NMEA_GPGLL) ? NMEA_rate : 0;
    //  msgs.gpgsa = (NMEA_message_configuration & NMEA_GPGSA) ? NMEA_rate : 0;
    //  msgs.gprmc = (NMEA_message_configuration & NMEA_GPRMC) ? NMEA_rate : 0;
    //  IS_.SendData(DID_ASCII_BCAST_PERIOD, (uint8_t*)(&msgs), sizeof(ascii_msgs_t), 0);
}

void InertialSenseROS::connect()
{
    nh_private_.param<std::string>("port", port_, "/dev/ttyACM0");
    nh_private_.param<int>("baudrate", baudrate_, 921600);
    nh_private_.param<std::string>("frame_id", frame_id_, "body");

    /// Connect to the uINS
    ROS_INFO("Connecting to serial port \"%s\", at %d baud", port_.c_str(), baudrate_);
    if (!IS_.Open(port_.c_str(), baudrate_))
    {
        ROS_FATAL("inertialsense: Unable to open serial port \"%s\", at %d baud", port_.c_str(), baudrate_);
        exit(0);
    }
    else
    {
        // Print if Successful
        ROS_INFO("Connected to uINS %d on \"%s\", at %d baud", IS_.GetDeviceInfo().serialNumber, port_.c_str(), baudrate_);
    }
}

void InertialSenseROS::set_navigation_dt_ms()
{
    // Make sure the navigation rate is right, if it's not, then we need to change and reset it.
    int nav_dt_ms = IS_.GetFlashConfig().startupNavDtMs;
    if (nh_private_.getParam("navigation_dt_ms", nav_dt_ms))
    {
        if (nav_dt_ms != IS_.GetFlashConfig().startupNavDtMs)
        {
            uint32_t data = nav_dt_ms;
            IS_.SendData(DID_FLASH_CONFIG, (uint8_t *)(&data), sizeof(uint32_t), offsetof(nvm_flash_cfg_t, startupNavDtMs));
            ROS_INFO("navigation rate change from %dms to %dms, resetting uINS to make change", IS_.GetFlashConfig().startupNavDtMs, nav_dt_ms);
            sleep(3);
            reset_device();
        }
    }
}

void InertialSenseROS::configure_flash_parameters()
{
    set_vector_flash_config<float>("INS_rpy_radians", 3, offsetof(nvm_flash_cfg_t, insRotation));
    set_vector_flash_config<float>("INS_xyz", 3, offsetof(nvm_flash_cfg_t, insOffset));
    set_vector_flash_config<float>("GPS_ant1_xyz", 3, offsetof(nvm_flash_cfg_t, gps1AntOffset));
    set_vector_flash_config<float>("GPS_ant2_xyz", 3, offsetof(nvm_flash_cfg_t, gps2AntOffset));
    set_vector_flash_config<double>("GPS_ref_lla", 3, offsetof(nvm_flash_cfg_t, refLla));

    set_flash_config<float>("inclination", offsetof(nvm_flash_cfg_t, magInclination), 0.0f);
    set_flash_config<float>("declination", offsetof(nvm_flash_cfg_t, magDeclination), 0.0f);
    set_flash_config<int>("dynamic_model", offsetof(nvm_flash_cfg_t, insDynModel), 8);
    // set_flash_config<int>("ser1_baud_rate", offsetof(nvm_flash_cfg_t, ser1BaudRate), 921600);
}

void InertialSenseROS::connect_rtk_client(const std::string &RTK_correction_protocol, const std::string &RTK_server_IP, const int RTK_server_port)
{
    rtk_connecting_ = true;

    std::string RTK_server_mount;
    std::string RTK_server_username;
    std::string RTK_server_password;

    int RTK_connection_attempt_limit;
    int RTK_connection_attempt_backoff;

    nh_private_.param<std::string>("RTK_server_mount", RTK_server_mount, "");
    nh_private_.param<std::string>("RTK_server_username", RTK_server_username, "");
    nh_private_.param<std::string>("RTK_server_password", RTK_server_password, "");

    nh_private_.param<int>("RTK_connection_attempt_limit", RTK_connection_attempt_limit, 1);
    nh_private_.param<int>("RTK_connection_attempt_backoff", RTK_connection_attempt_backoff, 2);

    // [type]:[protocol]:[ip/url]:[port]:[mountpoint]:[username]:[password]
    std::string RTK_connection = "TCP:" + RTK_correction_protocol + ":" + RTK_server_IP + ":" + std::to_string(RTK_server_port);
    if (!RTK_server_mount.empty() && !RTK_server_username.empty())
    { // NTRIP options
        RTK_connection += ":" + RTK_server_mount + ":" + RTK_server_username + ":" + RTK_server_password;
    }

    int RTK_connection_attempt_count = 0;
    while (RTK_connection_attempt_count < RTK_connection_attempt_limit)
    {
        ++RTK_connection_attempt_count;

        bool connected = IS_.OpenConnectionToServer(RTK_connection);

        if (connected)
        {
            ROS_INFO_STREAM("Successfully connected to " << RTK_connection << " RTK server");
            break;
        }
        else
        {
            ROS_ERROR_STREAM("Failed to connect to base server at " << RTK_connection);

            if (RTK_connection_attempt_count >= RTK_connection_attempt_limit)
            {
                ROS_ERROR_STREAM("Giving up after " << RTK_connection_attempt_count << " failed attempts");
            }
            else
            {
                int sleep_duration = RTK_connection_attempt_count * RTK_connection_attempt_backoff;
                ROS_WARN_STREAM("Retrying connection in " << sleep_duration << " seconds");
                ros::Duration(sleep_duration).sleep();
            }
        }
    }

    rtk_connecting_ = false;
}

void InertialSenseROS::start_rtk_server(const std::string &RTK_server_IP, const int RTK_server_port)
{
    // [type]:[ip/url]:[port]
    std::string RTK_connection = "TCP:" + RTK_server_IP + ":" + std::to_string(RTK_server_port);

    if (IS_.CreateHost(RTK_connection))
    {
        ROS_INFO_STREAM("Successfully created " << RTK_connection << " as RTK server");
        initialized_ = true;
        return;
    }
    else
        ROS_ERROR_STREAM("Failed to create base server at " << RTK_connection);
}

void InertialSenseROS::start_rtk_connectivity_watchdog_timer()
{
    bool rtk_connectivity_watchdog_enabled;
    // default is false for legacy compatibility
    nh_private_.param<bool>("RTK_connectivity_watchdog_enabled", rtk_connectivity_watchdog_enabled, false);
    if (!rtk_connectivity_watchdog_enabled)
    {
        return;
    }

    if (!rtk_connectivity_watchdog_timer_.isValid())
    {
        float rtk_connectivity_watchdog_timer_frequency;
        nh_private_.param<float>("RTK_connectivity_watchdog_timer_frequency", rtk_connectivity_watchdog_timer_frequency, 1);
        rtk_connectivity_watchdog_timer_ = nh_.createTimer(ros::Duration(rtk_connectivity_watchdog_timer_frequency), InertialSenseROS::rtk_connectivity_watchdog_timer_callback, this);
    }

    rtk_connectivity_watchdog_timer_.start();
}

void InertialSenseROS::stop_rtk_connectivity_watchdog_timer()
{
    rtk_traffic_total_byte_count_ = 0;
    rtk_data_transmission_interruption_count_ = 0;
    rtk_connectivity_watchdog_timer_.stop();
}

void InertialSenseROS::rtk_connectivity_watchdog_timer_callback(const ros::TimerEvent &timer_event)
{
    if (rtk_connecting_)
    {
        return;
    }

    int latest_byte_count = IS_.GetClientServerByteCount();
    if (rtk_traffic_total_byte_count_ == latest_byte_count)
    {
        ++rtk_data_transmission_interruption_count_;

        int rtk_data_transmission_interruption_limit_;
        nh_private_.param<int>("RTK_data_transmission_interruption_limit", rtk_data_transmission_interruption_limit_, 5);
        if (rtk_data_transmission_interruption_count_ >= rtk_data_transmission_interruption_limit_)
        {
            ROS_WARN("RTK transmission interruption, reconnecting...");

            std::string RTK_correction_protocol;
            std::string RTK_server_IP;
            int RTK_server_port;
            nh_private_.param<std::string>("RTK_correction_protocol", RTK_correction_protocol, "RTCM3");
            nh_private_.param<std::string>("RTK_server_IP", RTK_server_IP, "127.0.0.1");
            nh_private_.param<int>("RTK_server_port", RTK_server_port, 7777);

            connect_rtk_client(RTK_correction_protocol, RTK_server_IP, RTK_server_port);
        }
    }
    else
    {
        rtk_traffic_total_byte_count_ = latest_byte_count;
        rtk_data_transmission_interruption_count_ = 0;
    }
}

void InertialSenseROS::configure_rtk()
{
    bool RTK_rover, RTK_rover_radio_enable, RTK_base, dual_GNSS;
    std::string gps_type;
    nh_private_.param<std::string>("gps_type", gps_type, "M8");
    nh_private_.param<bool>("RTK_rover", RTK_rover, false);
    nh_private_.param<bool>("RTK_rover_radio_enable", RTK_rover_radio_enable, false);
    nh_private_.param<bool>("RTK_base", RTK_base, false);
    nh_private_.param<bool>("dual_GNSS", dual_GNSS, false);

    std::string RTK_correction_protocol;
    std::string RTK_server_IP;
    int RTK_server_port;
    nh_private_.param<std::string>("RTK_correction_protocol", RTK_correction_protocol, "RTCM3");
    nh_private_.param<std::string>("RTK_server_IP", RTK_server_IP, "127.0.0.1");
    nh_private_.param<int>("RTK_server_port", RTK_server_port, 7777);

    ROS_ERROR_COND(RTK_rover && RTK_base, "unable to configure uINS to be both RTK rover and base - default to rover");
    ROS_ERROR_COND(RTK_rover && dual_GNSS, "unable to configure uINS to be both RTK rover as dual GNSS - default to dual GNSS");

    uint32_t RTKCfgBits = 0;
    if (dual_GNSS)
    {
        RTK_rover = false;
        ROS_INFO("InertialSense: Configured as dual GNSS (compassing)");
        RTK_state_ = DUAL_GNSS;
        RTKCfgBits |= RTK_CFG_BITS_ROVER_MODE_RTK_COMPASSING;
        SET_CALLBACK(DID_GPS2_RTK_CMP_MISC, gps_rtk_misc_t, RTK_Misc_callback, 1);
        SET_CALLBACK(DID_GPS2_RTK_CMP_REL, gps_rtk_rel_t, RTK_Rel_callback, 1);
        RTK_.enabled = true;
        RTK_.pub = nh_.advertise<inertial_sense_ros::RTKInfo>("RTK/info", 10);
        RTK_.pub2 = nh_.advertise<inertial_sense_ros::RTKRel>("RTK/rel", 10);
    }

    if (RTK_rover_radio_enable)
    {
        RTK_base = false;
        ROS_INFO("InertialSense: Configured as RTK Rover with radio enabled");
        RTK_state_ = RTK_ROVER;
        RTKCfgBits |= (gps_type == "F9P" ? RTK_CFG_BITS_ROVER_MODE_RTK_POSITIONING_EXTERNAL : RTK_CFG_BITS_ROVER_MODE_RTK_POSITIONING);

        SET_CALLBACK(DID_GPS1_RTK_POS_MISC, gps_rtk_misc_t, RTK_Misc_callback, 1);
        SET_CALLBACK(DID_GPS1_RTK_POS_REL, gps_rtk_rel_t, RTK_Rel_callback, 1);
        RTK_.enabled = true;
        RTK_.pub = nh_.advertise<inertial_sense_ros::RTKInfo>("RTK/info", 10);
        RTK_.pub2 = nh_.advertise<inertial_sense_ros::RTKRel>("RTK/rel", 10);
    }
    else if (RTK_rover)
    {
        RTK_base = false;

        ROS_INFO("InertialSense: Configured as RTK Rover");
        RTK_state_ = RTK_ROVER;
        RTKCfgBits |= (gps_type == "F9P" ? RTK_CFG_BITS_ROVER_MODE_RTK_POSITIONING_EXTERNAL : RTK_CFG_BITS_ROVER_MODE_RTK_POSITIONING);

        connect_rtk_client(RTK_correction_protocol, RTK_server_IP, RTK_server_port);

        SET_CALLBACK(DID_GPS1_RTK_POS_MISC, gps_rtk_misc_t, RTK_Misc_callback, 1);
        SET_CALLBACK(DID_GPS1_RTK_POS_REL, gps_rtk_rel_t, RTK_Rel_callback, 1);
        RTK_.enabled = true;
        RTK_.pub = nh_.advertise<inertial_sense_ros::RTKInfo>("RTK/info", 10);
        RTK_.pub2 = nh_.advertise<inertial_sense_ros::RTKRel>("RTK/rel", 10);

        start_rtk_connectivity_watchdog_timer();
    }
    else if (RTK_base)
    {
        RTK_.enabled = true;
        ROS_INFO("InertialSense: Configured as RTK Base");
        RTK_state_ = RTK_BASE;
        RTKCfgBits |= RTK_CFG_BITS_BASE_OUTPUT_GPS1_UBLOX_SER0;

        start_rtk_server(RTK_server_IP, RTK_server_port);
    }
    IS_.SendData(DID_FLASH_CONFIG, reinterpret_cast<uint8_t *>(&RTKCfgBits), sizeof(RTKCfgBits), offsetof(nvm_flash_cfg_t, RTKCfgBits));
}

template <typename T>
void InertialSenseROS::set_vector_flash_config(std::string param_name, uint32_t size, uint32_t offset)
{
    std::vector<double> tmp(size, 0);
    T v[size];
    if (!nh_private_.hasParam(param_name))
    {   // Parameter not provided.
        return;
    }

    nh_private_.getParam(param_name, tmp);

    for (int i = 0; i < size; i++)
    {
        v[i] = tmp[i];
    }

    IS_.SendData(DID_FLASH_CONFIG, reinterpret_cast<uint8_t *>(&v), sizeof(v), offset);
    IS_.GetFlashConfig() = IS_.GetFlashConfig();
}

template <typename T>
void InertialSenseROS::set_flash_config(std::string param_name, uint32_t offset, T def)
{
    T tmp;
    nh_private_.param<T>(param_name, tmp, def);
    IS_.SendData(DID_FLASH_CONFIG, reinterpret_cast<uint8_t *>(&tmp), sizeof(T), offset);
}

void InertialSenseROS::flash_config_callback(const nvm_flash_cfg_t *const msg)
{
    refLla_[0] = msg->refLla[0];
    refLla_[1] = msg->refLla[1];
    refLla_[2] = msg->refLla[2];
    refLLA_known = true;
}

void InertialSenseROS::INS1_callback(const ins_1_t *const msg)
{
    // Standard DID_INS_1 message
    if (DID_INS_1_.enabled)
    {
        did_ins_1_msg.header.stamp = ros_time_from_week_and_tow(msg->week, msg->timeOfWeek);
        did_ins_1_msg.header.frame_id = frame_id_;
        did_ins_1_msg.week = msg->week;
        did_ins_1_msg.timeOfWeek = msg->timeOfWeek;
        did_ins_1_msg.insStatus = msg->insStatus;
        did_ins_1_msg.hdwStatus = msg->hdwStatus;
        did_ins_1_msg.theta[0] = msg->theta[0];
        did_ins_1_msg.theta[1] = msg->theta[1];
        did_ins_1_msg.theta[2] = msg->theta[2];
        did_ins_1_msg.uvw[0] = msg->uvw[0];
        did_ins_1_msg.uvw[1] = msg->uvw[1];
        did_ins_1_msg.uvw[2] = msg->uvw[2];
        did_ins_1_msg.lla[0] = msg->lla[0];
        did_ins_1_msg.lla[1] = msg->lla[1];
        did_ins_1_msg.lla[2] = msg->lla[2];
        did_ins_1_msg.ned[0] = msg->ned[0];
        did_ins_1_msg.ned[1] = msg->ned[1];
        did_ins_1_msg.ned[2] = msg->ned[2];
        DID_INS_1_.pub.publish(did_ins_1_msg);
    }
}

void InertialSenseROS::INS2_callback(const ins_2_t *const msg)
{
    if (DID_INS_2_.enabled)
    {
        // Standard DID_INS_2 message
        did_ins_2_msg.header.frame_id = frame_id_;
        did_ins_2_msg.week = msg->week;
        did_ins_2_msg.timeOfWeek = msg->timeOfWeek;
        did_ins_2_msg.insStatus = msg->insStatus;
        did_ins_2_msg.hdwStatus = msg->hdwStatus;
        did_ins_2_msg.qn2b[0] = msg->qn2b[0];
        did_ins_2_msg.qn2b[1] = msg->qn2b[1];
        did_ins_2_msg.qn2b[2] = msg->qn2b[2];
        did_ins_2_msg.qn2b[3] = msg->qn2b[3];
        did_ins_2_msg.uvw[0] = msg->uvw[0];
        did_ins_2_msg.uvw[1] = msg->uvw[1];
        did_ins_2_msg.uvw[2] = msg->uvw[2];
        did_ins_2_msg.lla[0] = msg->lla[0];
        did_ins_2_msg.lla[1] = msg->lla[1];
        did_ins_2_msg.lla[2] = msg->lla[2];
        DID_INS_2_.pub.publish(did_ins_2_msg);
    }
}

void InertialSenseROS::INS4_callback(const ins_4_t *const msg)
{
    if (!refLLA_known)
    {
        ROS_INFO("REFERENCE LLA MUST BE RECEIVED");
        return;
    }
    if (DID_INS_4_.enabled)
    {
        // Standard DID_INS_2 message
        did_ins_4_msg.header.frame_id = frame_id_;
        did_ins_4_msg.week = msg->week;
        did_ins_4_msg.timeOfWeek = msg->timeOfWeek;
        did_ins_4_msg.insStatus = msg->insStatus;
        did_ins_4_msg.hdwStatus = msg->hdwStatus;
        did_ins_4_msg.qe2b[0] = msg->qe2b[0];
        did_ins_4_msg.qe2b[1] = msg->qe2b[1];
        did_ins_4_msg.qe2b[2] = msg->qe2b[2];
        did_ins_4_msg.qe2b[3] = msg->qe2b[3];
        did_ins_4_msg.ve[0] = msg->ve[0];
        did_ins_4_msg.ve[1] = msg->ve[1];
        did_ins_4_msg.ve[2] = msg->ve[2];
        did_ins_4_msg.ecef[0] = msg->ecef[0];
        did_ins_4_msg.ecef[1] = msg->ecef[1];
        did_ins_4_msg.ecef[2] = msg->ecef[2];
        DID_INS_4_.pub.publish(did_ins_4_msg);
    }

    if (odom_ins_ned_.enabled || odom_ins_enu_.enabled || odom_ins_ecef_.enabled)
    {
        // Note: the covariance matrices need to be transformed into required frames of reference before publishing the ROS message!
        ixMatrix3  Rb2e, I;
        ixVector4  qe2b, qe2n;
        ixVector3d Pe, lla;
        float Pout[36];

        eye_MatN(I, 3);
        qe2b[0] = msg->qe2b[0];
        qe2b[1] = msg->qe2b[1];
        qe2b[2] = msg->qe2b[2];
        qe2b[3] = msg->qe2b[3];

        rotMatB2R(qe2b, Rb2e);
        Pe[0] = msg->ecef[0];
        Pe[1] = msg->ecef[1];
        Pe[2] = msg->ecef[2];
        ecef2lla(Pe, lla, 5);
        quat_ecef2ned(lla[0], lla[1], qe2n);


        if (odom_ins_ecef_.enabled)
        {
            // Pose
            // Transform attitude body to ECEF
            transform_6x6_covariance(Pout, poseCov, I, Rb2e);
            for (int i = 0; i < 36; i++) 
            {
                ecef_odom_msg.pose.covariance[i] = Pout[i];
            }
            // Twist
            // Transform angular_rate from body to ECEF
            transform_6x6_covariance(Pout, twistCov, I, Rb2e);
            for (int i = 0; i < 36; i++) 
            {
                ecef_odom_msg.twist.covariance[i] = Pout[i];
            }
            ecef_odom_msg.header.stamp = ros_time_from_week_and_tow(msg->week, msg->timeOfWeek);
            ecef_odom_msg.header.frame_id = frame_id_;

            // Position

            ecef_odom_msg.pose.pose.position.x = msg->ecef[0];
            ecef_odom_msg.pose.pose.position.y = msg->ecef[1];
            ecef_odom_msg.pose.pose.position.z = -msg->ecef[2];

            // Attitude

            ecef_odom_msg.pose.pose.orientation.w = msg->qe2b[0];
            ecef_odom_msg.pose.pose.orientation.x = msg->qe2b[1];
            ecef_odom_msg.pose.pose.orientation.y = msg->qe2b[2];
            ecef_odom_msg.pose.pose.orientation.z = msg->qe2b[3];

            // Linear Velocity

            ecef_odom_msg.twist.twist.linear.x = msg->ve[0];
            ecef_odom_msg.twist.twist.linear.y = msg->ve[1];
            ecef_odom_msg.twist.twist.linear.z = msg->ve[2];

            // Angular Velocity
            ixVector3 result;
            ixEuler theta;
            quat2euler(msg->qe2b, theta);
            ixVector3 angVelImu = {(f_t)imu_msg.angular_velocity.x, (f_t)imu_msg.angular_velocity.y, (f_t)imu_msg.angular_velocity.z};
            vectorBodyToReference(angVelImu, theta, result);

            ecef_odom_msg.twist.twist.angular.x = result[0];
            ecef_odom_msg.twist.twist.angular.y = result[1];
            ecef_odom_msg.twist.twist.angular.z = result[2];

            odom_ins_ecef_.pub.publish(ecef_odom_msg);

            if (publishTf)
            {
                // Calculate the TF from the pose...
                transform_ECEF.setOrigin(tf::Vector3(ecef_odom_msg.pose.pose.position.x, ecef_odom_msg.pose.pose.position.y, ecef_odom_msg.pose.pose.position.z));
                tf::Quaternion q;
                tf::quaternionMsgToTF(ecef_odom_msg.pose.pose.orientation, q);
                transform_ECEF.setRotation(q);

                br.sendTransform(tf::StampedTransform(transform_ECEF, ros::Time::now(), "ins_ecef", "ins_base_link_ecef"));
            }
        }

        if (odom_ins_ned_.enabled)
        {
            ixVector4 qn2b;
            ixMatrix3 Rb2n, Re2n, buf;

            // NED-to-body quaternion
            mul_Quat_ConjQuat(qn2b, qe2b, qe2n);
            // Body-to-NED rotation matrix
            rotMatB2R(qn2b, Rb2n);
            // ECEF-to-NED rotation matrix
            rotMatB2R(qe2n, buf);
            transpose_Mat3(Re2n, buf);

            // Pose
            // Transform position from ECEF to NED and attitude from body to NED
            transform_6x6_covariance(Pout, poseCov, Re2n, Rb2n);
            for (int i = 0; i < 36; i++) 
            {
                ned_odom_msg.pose.covariance[i] = Pout[i];
            }
            // Twist
            // Transform velocity from ECEF to NED and angular rate from body to NED
            transform_6x6_covariance(Pout, twistCov, Re2n, Rb2n);
            for (int i = 0; i < 36; i++) 
            {
                ned_odom_msg.twist.covariance[i] = Pout[i];
            }

            ned_odom_msg.header.stamp = ros_time_from_week_and_tow(msg->week, msg->timeOfWeek);
            ned_odom_msg.header.frame_id = frame_id_;
                                                                
            // Position
            ixVector3d llaPosRadians;
                //ecef to lla (rad,rad,m)
            ecef2lla(msg->ecef, llaPosRadians, 5);
            ixVector3 ned;
            ixVector3d refLlaRadians;
                //convert refLla_ to radians
            lla_Deg2Rad_d(refLlaRadians, refLla_);
                //lla to ned
            lla2ned_d(refLlaRadians, llaPosRadians, ned);

            ned_odom_msg.pose.pose.position.x = ned[0];
            ned_odom_msg.pose.pose.position.y = ned[1];
            ned_odom_msg.pose.pose.position.z = ned[2];

            // Attitude

            ned_odom_msg.pose.pose.orientation.w = qn2b[0]; // w
            ned_odom_msg.pose.pose.orientation.x = qn2b[1]; // x
            ned_odom_msg.pose.pose.orientation.y = qn2b[2]; // y
            ned_odom_msg.pose.pose.orientation.z = qn2b[3]; // z

            // Linear Velocity
            ixVector3 result, theta;

            quatConjRot(result, qe2n, msg->ve);            

            ned_odom_msg.twist.twist.linear.x = result[0];
            ned_odom_msg.twist.twist.linear.y = result[1];
            ned_odom_msg.twist.twist.linear.z = result[2];

            // Angular Velocity

            // Transform from body frame to NED
            ixVector3 angVelImu = {(f_t)imu_msg.angular_velocity.x, (f_t)imu_msg.angular_velocity.y, (f_t)imu_msg.angular_velocity.z};
            quatRot(result, qn2b, angVelImu);

            ned_odom_msg.twist.twist.angular.x = result[0];
            ned_odom_msg.twist.twist.angular.y = result[1];
            ned_odom_msg.twist.twist.angular.z = result[2];
            odom_ins_ned_.pub.publish(ned_odom_msg);
            
            if (publishTf)
            {
                // Calculate the TF from the pose...
                transform_NED.setOrigin(tf::Vector3(ned_odom_msg.pose.pose.position.x, ned_odom_msg.pose.pose.position.y, ned_odom_msg.pose.pose.position.z));
                tf::Quaternion q;
                tf::quaternionMsgToTF(ned_odom_msg.pose.pose.orientation, q);
                transform_NED.setRotation(q);

                br.sendTransform(tf::StampedTransform(transform_NED, ros::Time::now(), "ins_ned", "ins_base_link_ned"));
            }
        }

        if (odom_ins_enu_.enabled)
        {
            ixVector4 qn2b, qn2enu, qe2enu, qenu2b;
            ixMatrix3 Rb2enu, Re2enu, buf;
            ixEuler eul = {M_PI, 0, 0.5 * M_PI};
            // ENU-to-NED quaternion
            euler2quat(eul, qn2enu);
            // NED-to-body quaternion
            mul_Quat_ConjQuat(qn2b, qe2b, qe2n);
            // ENU-to-body quaternion
            mul_Quat_ConjQuat(qenu2b, qn2b, qn2enu);
            // ECEF-to-ENU quaternion
            mul_Quat_Quat(qe2enu, qn2enu, qe2n);
            // Body-to-ENU rotation matrix
            rotMatB2R(qenu2b, Rb2enu);
            // ECEF-to-ENU rotation matrix
            rotMatB2R(qe2enu, buf);
            transpose_Mat3(Re2enu, buf);

            // Pose
            // Transform position from ECEF to ENU and attitude from body to ENU
            transform_6x6_covariance(Pout, poseCov, Re2enu, Rb2enu);
            for (int i = 0; i < 36; i++) 
            {
                enu_odom_msg.pose.covariance[i] = Pout[i];
            }
            // Twist
            // Transform velocity from ECEF to ENU and angular rate from body to ENU
            transform_6x6_covariance(Pout, twistCov, Re2enu, Rb2enu);
            for (int i = 0; i < 36; i++) 
            {
                enu_odom_msg.twist.covariance[i] = Pout[i];
            }
            
            enu_odom_msg.header.stamp = ros_time_from_week_and_tow(msg->week, msg->timeOfWeek);
            enu_odom_msg.header.frame_id = frame_id_;

            // Position
                //Calculate in NED then convert
            ixVector3d llaPosRadians;
                //ecef to lla (rad,rad,m)
            ecef2lla(msg->ecef, llaPosRadians, 5);
            ixVector3 ned;
            ixVector3d refLlaRadians;
                //convert refLla_ to radians
            lla_Deg2Rad_d(refLlaRadians, refLla_);
                //lla to ned
            lla2ned_d(refLlaRadians, llaPosRadians, ned);

            // Rearrange from NED to ENU
            enu_odom_msg.pose.pose.position.x = ned[1];
            enu_odom_msg.pose.pose.position.y = ned[0];
            enu_odom_msg.pose.pose.position.z = -ned[2];

            // Attitude

            enu_odom_msg.pose.pose.orientation.w = qenu2b[0];
            enu_odom_msg.pose.pose.orientation.x = qenu2b[1];
            enu_odom_msg.pose.pose.orientation.y = qenu2b[2];
            enu_odom_msg.pose.pose.orientation.z = qenu2b[3];

            // Linear Velocity
                //same as NED but rearranged.
            ixVector3 result, theta;
            quatConjRot(result, qe2n, msg->ve);            

            enu_odom_msg.twist.twist.linear.x = result[1];
            enu_odom_msg.twist.twist.linear.y = result[0];
            enu_odom_msg.twist.twist.linear.z = -result[2];

            // Angular Velocity

            // Transform from body frame to ENU
            ixVector3 angVelImu = {(f_t)imu_msg.angular_velocity.x, (f_t)imu_msg.angular_velocity.y, (f_t)imu_msg.angular_velocity.z};
            quatRot(result, qenu2b, angVelImu);

            enu_odom_msg.twist.twist.angular.x = result[0];
            enu_odom_msg.twist.twist.angular.y = result[1];
            enu_odom_msg.twist.twist.angular.z = result[2];

            odom_ins_enu_.pub.publish(enu_odom_msg);
            if (publishTf)
            {
                // Calculate the TF from the pose...
                transform_ENU.setOrigin(tf::Vector3(enu_odom_msg.pose.pose.position.x, enu_odom_msg.pose.pose.position.y, enu_odom_msg.pose.pose.position.z));
                tf::Quaternion q;
                tf::quaternionMsgToTF(enu_odom_msg.pose.pose.orientation, q);
                transform_ENU.setRotation(q);

                br.sendTransform(tf::StampedTransform(transform_ENU, ros::Time::now(), "ins_enu", "ins_base_link_enu"));
            }
        }
    }
}

void InertialSenseROS::INL2_states_callback(const inl2_states_t *const msg)
{
    inl2_states_msg.header.stamp = ros_time_from_tow(msg->timeOfWeek);
    inl2_states_msg.header.frame_id = frame_id_;

    inl2_states_msg.quatEcef.w = msg->qe2b[0];
    inl2_states_msg.quatEcef.x = msg->qe2b[1];
    inl2_states_msg.quatEcef.y = msg->qe2b[2];
    inl2_states_msg.quatEcef.z = msg->qe2b[3];

    inl2_states_msg.velEcef.x = msg->ve[0];
    inl2_states_msg.velEcef.y = msg->ve[1];
    inl2_states_msg.velEcef.z = msg->ve[2];

    inl2_states_msg.posEcef.x = msg->ecef[0];
    inl2_states_msg.posEcef.y = msg->ecef[1];
    inl2_states_msg.posEcef.z = msg->ecef[2];

    inl2_states_msg.gyroBias.x = msg->biasPqr[0];
    inl2_states_msg.gyroBias.y = msg->biasPqr[1];
    inl2_states_msg.gyroBias.z = msg->biasPqr[2];

    inl2_states_msg.accelBias.x = msg->biasAcc[0];
    inl2_states_msg.accelBias.y = msg->biasAcc[1];
    inl2_states_msg.accelBias.z = msg->biasAcc[2];

    inl2_states_msg.baroBias = msg->biasBaro;
    inl2_states_msg.magDec = msg->magDec;
    inl2_states_msg.magInc = msg->magInc;

    // Use custom INL2 states message
    if (INL2_states_.enabled)
    {
        INL2_states_.pub.publish(inl2_states_msg);
    }
}

void InertialSenseROS::INS_covariance_callback(const ros_covariance_pose_twist_t *const msg)
{
    float poseCovIn[36];
    int ind1, ind2;

    // Pose and twist covariances unwrapped from LD
    LD2Cov(msg->covPoseLD, poseCovIn, 6);
    LD2Cov(msg->covTwistLD, twistCov, 6);

    // Need to change order of variables. 
    // Incoming order for msg->covPoseLD is [attitude, position]. Outgoing should be [position, attitude] => need to swap
    // Incoming order for msg->covTwistLD is [lin_velocity, ang_rate]. Outgoing should be [lin_velocity, ang_rate] => no change
    // Order change (block swap) in covariance matrix:
    // |A  C| => |B  C'|
    // |C' B|    |C  A |
    // where A and B are symetric, C' is transposed C

    for (int i = 0; i < 3; i++) {
        for (int j = 0; j <= i; j++) {
            // Swap blocks A and B
            ind1 = (i + 3) * 6 + j + 3;
            ind2 = i * 6 + j;
            poseCov[ind2] = poseCovIn[ind1];
            poseCov[ind1] = poseCovIn[ind2];
            if (i != j) {
                // Copy lower diagonals to upper diagonals
                poseCov[j * 6 + i] = poseCov[ind2];
                poseCov[(j + 3) * 6 + (i + 3)] = poseCov[ind1];
            }
        }
        // Swap blocks C and C'
        for (int j = 0; j < 3; j++) {
            ind1 = (i + 3) * 6 + j;
            ind2 = i * 6 + j + 3;
            poseCov[ind2] = poseCovIn[ind1];
            poseCov[ind1] = poseCovIn[ind2];
        }
    }
}


void InertialSenseROS::GPS_pos_callback(const gps_pos_t *const msg)
{
    GPS_week_ = msg->week;
    GPS_towOffset_ = msg->towOffset;
    if (GPS_.enabled && msg->status & GPS_STATUS_FIX_MASK)
    {
        gps_msg.header.stamp = ros_time_from_week_and_tow(msg->week, msg->timeOfWeekMs / 1.0e3);
        gps_msg.week = msg->week;
        gps_msg.status = msg->status;
        gps_msg.header.frame_id = frame_id_;
        gps_msg.num_sat = (uint8_t)(msg->status & GPS_STATUS_NUM_SATS_USED_MASK);
        gps_msg.cno = msg->cnoMean;
        gps_msg.latitude = msg->lla[0];
        gps_msg.longitude = msg->lla[1];
        gps_msg.altitude = msg->lla[2];
        gps_msg.posEcef.x = ecef_[0] = msg->ecef[0];
        gps_msg.posEcef.y = ecef_[1] = msg->ecef[1];
        gps_msg.posEcef.z = ecef_[2] = msg->ecef[2];
        gps_msg.hMSL = msg->hMSL;
        gps_msg.hAcc = msg->hAcc;
        gps_msg.vAcc = msg->vAcc;
        gps_msg.pDop = msg->pDop;
        publishGPS();
    }
    if (NavSatFix_.enabled)
    {
        NavSatFix_msg.header.stamp = ros_time_from_week_and_tow(msg->week, msg->timeOfWeekMs / 1.0e3);
        NavSatFix_msg.header.frame_id = frame_id_;
        NavSatFix_msg.status.status = -1;                           // Assume no Fix
        if (msg->status & GPS_STATUS_FIX_MASK >= GPS_STATUS_FIX_2D) // Check for fix and set
        {
            NavSatFix_msg.status.status = NavSatFixStatusFixType::STATUS_FIX;
        }

        if (msg->status & GPS_STATUS_FIX_SBAS) // Check for SBAS only fix
        {
            NavSatFix_msg.status.status = NavSatFixStatusFixType::STATUS_SBAS_FIX;
        }

        if (msg->status & GPS_STATUS_FIX_MASK >= GPS_STATUS_FIX_RTK_SINGLE) // Check for any RTK fix
        {
            NavSatFix_msg.status.status = NavSatFixStatusFixType::STATUS_GBAS_FIX;
        }

        // NavSatFix_msg.status.service - Service set at Node Startup
        NavSatFix_msg.latitude = msg->lla[0];
        NavSatFix_msg.longitude = msg->lla[1];
        NavSatFix_msg.altitude = msg->lla[2];

        // Diagonal Known
        const double varH = pow(msg->hAcc / 1000.0, 2);
        const double varV = pow(msg->vAcc / 1000.0, 2);
        NavSatFix_msg.position_covariance[0] = varH;
        NavSatFix_msg.position_covariance[4] = varH;
        NavSatFix_msg.position_covariance[8] = varV;
        NavSatFix_msg.position_covariance_type = COVARIANCE_TYPE_DIAGONAL_KNOWN;
        NavSatFix_.pub.publish(NavSatFix_msg);
    }
}

void InertialSenseROS::GPS_vel_callback(const gps_vel_t *const msg)
{
    if (GPS_.enabled && abs(GPS_towOffset_) > 0.001)
    {
        gps_velEcef.header.stamp = ros_time_from_week_and_tow(GPS_week_, msg->timeOfWeekMs / 1.0e3);
        gps_velEcef.vector.x = msg->vel[0];
        gps_velEcef.vector.y = msg->vel[1];
        gps_velEcef.vector.z = msg->vel[2];
        publishGPS();
    }
}

void InertialSenseROS::publishGPS()
{
    double dt = (gps_velEcef.header.stamp - gps_msg.header.stamp).toSec();
    if (abs(dt) < 2.0e-3)
    {
        gps_msg.velEcef = gps_velEcef.vector;
        GPS_.pub.publish(gps_msg);
    }
}

void InertialSenseROS::update()
{
    IS_.Update();
}

void InertialSenseROS::strobe_in_time_callback(const strobe_in_time_t *const msg)
{
    // create the subscriber if it doesn't exist
    if (strobe_pub_.getTopic().empty())
        strobe_pub_ = nh_.advertise<std_msgs::Header>("strobe_time", 1);

    if (abs(GPS_towOffset_) > 0.001)
    {
        std_msgs::Header strobe_msg;
        strobe_msg.stamp = ros_time_from_week_and_tow(msg->week, msg->timeOfWeekMs * 1.0e-3);
        strobe_pub_.publish(strobe_msg);
    }
}

void InertialSenseROS::GPS_info_callback(const gps_sat_t *const msg)
{
    if (abs(GPS_towOffset_) < 0.001)
    { // Wait for valid msg->timeOfWeekMs
        return;
    }

    gps_info_msg.header.stamp = ros_time_from_tow(msg->timeOfWeekMs / 1.0e3);
    gps_info_msg.header.frame_id = frame_id_;
    gps_info_msg.num_sats = msg->numSats;
    for (int i = 0; i < 50; i++)
    {
        gps_info_msg.sattelite_info[i].sat_id = msg->sat[i].svId;
        gps_info_msg.sattelite_info[i].cno = msg->sat[i].cno;
    }
    GPS_info_.pub.publish(gps_info_msg);
}

void InertialSenseROS::mag_callback(const magnetometer_t *const msg)
{
    sensor_msgs::MagneticField mag_msg;
    mag_msg.header.stamp = ros_time_from_start_time(msg->time);
    mag_msg.header.frame_id = frame_id_;
    mag_msg.magnetic_field.x = msg->mag[0];
    mag_msg.magnetic_field.y = msg->mag[1];
    mag_msg.magnetic_field.z = msg->mag[2];

    mag_.pub.publish(mag_msg);
}

void InertialSenseROS::baro_callback(const barometer_t *const msg)
{
    sensor_msgs::FluidPressure baro_msg;
    baro_msg.header.stamp = ros_time_from_start_time(msg->time);
    baro_msg.header.frame_id = frame_id_;
    baro_msg.fluid_pressure = msg->bar;
    baro_msg.variance = msg->barTemp;

    baro_.pub.publish(baro_msg);
}

void InertialSenseROS::preint_IMU_callback(const preintegrated_imu_t *const msg)
{
    if (preint_IMU_.enabled)
    {
        preintIMU_msg.header.stamp = ros_time_from_start_time(msg->time);
        preintIMU_msg.header.frame_id = frame_id_;
        preintIMU_msg.dtheta.x = (msg->theta1[0] + msg->theta2[0]) / 2;
        preintIMU_msg.dtheta.y = (msg->theta1[1] + msg->theta2[1]) / 2;
        preintIMU_msg.dtheta.z = (msg->theta1[2] + msg->theta2[2]) / 2;

        preintIMU_msg.dvel.x = (msg->vel1[0] + msg->vel2[0]) / 2;
        preintIMU_msg.dvel.y = (msg->vel1[1] + msg->vel2[1]) / 2;
        preintIMU_msg.dvel.z = (msg->vel1[2] + msg->vel2[2]) / 2;

        preintIMU_msg.dt = msg->dt;

        preint_IMU_.pub.publish(preintIMU_msg);
    }
    
    if (IMU_.enabled)
    {
        imu_msg.header.stamp = ros_time_from_start_time(msg->time);
        imu_msg.header.frame_id = frame_id_;

        imu_msg.angular_velocity.x = ((msg->theta1[0] + msg->theta2[0]) / 2) /msg->dt;
        imu_msg.angular_velocity.y = ((msg->theta1[1] + msg->theta2[1]) / 2) /msg->dt;
        imu_msg.angular_velocity.z = ((msg->theta1[2] + msg->theta2[2]) / 2) /msg->dt;
        imu_msg.linear_acceleration.x = ((msg->vel1[0] + msg->vel2[0]) / 2) /msg->dt;
        imu_msg.linear_acceleration.y = ((msg->vel1[1] + msg->vel2[1]) / 2) /msg->dt;
        imu_msg.linear_acceleration.z = ((msg->vel1[2] + msg->vel2[2]) / 2) /msg->dt;

        IMU_.pub.publish(imu_msg);
    }
}

void InertialSenseROS::RTK_Misc_callback(const gps_rtk_misc_t *const msg)
{
    if (RTK_.enabled && abs(GPS_towOffset_) > 0.001)
    {
        inertial_sense_ros::RTKInfo rtk_info;
        rtk_info.header.stamp = ros_time_from_week_and_tow(GPS_week_, msg->timeOfWeekMs / 1000.0);
        rtk_info.baseAntcount = msg->baseAntennaCount;
        rtk_info.baseEph = msg->baseBeidouEphemerisCount + msg->baseGalileoEphemerisCount + msg->baseGlonassEphemerisCount + msg->baseGpsEphemerisCount;
        rtk_info.baseObs = msg->baseBeidouObservationCount + msg->baseGalileoObservationCount + msg->baseGlonassObservationCount + msg->baseGpsObservationCount;
        rtk_info.BaseLLA[0] = msg->baseLla[0];
        rtk_info.BaseLLA[1] = msg->baseLla[1];
        rtk_info.BaseLLA[2] = msg->baseLla[2];

        rtk_info.roverEph = msg->roverBeidouEphemerisCount + msg->roverGalileoEphemerisCount + msg->roverGlonassEphemerisCount + msg->roverGpsEphemerisCount;
        rtk_info.roverObs = msg->roverBeidouObservationCount + msg->roverGalileoObservationCount + msg->roverGlonassObservationCount + msg->roverGpsObservationCount;
        rtk_info.cycle_slip_count = msg->cycleSlipCount;
        RTK_.pub.publish(rtk_info);
    }
}

void InertialSenseROS::RTK_Rel_callback(const gps_rtk_rel_t *const msg)
{
    if (RTK_.enabled && abs(GPS_towOffset_) > 0.001)
    {
        inertial_sense_ros::RTKRel rtk_rel;
        rtk_rel.header.stamp = ros_time_from_week_and_tow(GPS_week_, msg->timeOfWeekMs / 1000.0);
        rtk_rel.differential_age = msg->differentialAge;
        rtk_rel.ar_ratio = msg->arRatio;
        uint32_t fixStatus = msg->status & GPS_STATUS_FIX_MASK;
        if (fixStatus == GPS_STATUS_FIX_3D)
        {
            rtk_rel.eGpsNavFixStatus = inertial_sense_ros::RTKRel::GPS_STATUS_FIX_3D;
        }
        else if (fixStatus == GPS_STATUS_FIX_RTK_SINGLE)
        {
            rtk_rel.eGpsNavFixStatus = inertial_sense_ros::RTKRel::GPS_STATUS_FIX_RTK_SINGLE;
        }
        else if (fixStatus == GPS_STATUS_FIX_RTK_FLOAT)
        {
            rtk_rel.eGpsNavFixStatus = inertial_sense_ros::RTKRel::GPS_STATUS_FIX_RTK_FLOAT;
        }
        else if (fixStatus == GPS_STATUS_FIX_RTK_FIX)
        {
            rtk_rel.eGpsNavFixStatus = inertial_sense_ros::RTKRel::GPS_STATUS_FIX_RTK_FIX;
        }
        else if (msg->status & GPS_STATUS_FLAGS_RTK_FIX_AND_HOLD)
        {
            rtk_rel.eGpsNavFixStatus = inertial_sense_ros::RTKRel::GPS_STATUS_FLAGS_RTK_FIX_AND_HOLD;
        }
        
        
        rtk_rel.vector_base_to_rover.x = msg->baseToRoverVector[0];
        rtk_rel.vector_base_to_rover.y = msg->baseToRoverVector[1];
        rtk_rel.vector_base_to_rover.z = msg->baseToRoverVector[2];
        rtk_rel.distance_base_to_rover = msg->baseToRoverDistance;
        rtk_rel.heading_base_to_rover = msg->baseToRoverHeading;
        RTK_.pub2.publish(rtk_rel);

        // save for diagnostics
        diagnostic_ar_ratio_ = rtk_rel.ar_ratio;
        diagnostic_differential_age_ = rtk_rel.differential_age;
        diagnostic_heading_base_to_rover_ = rtk_rel.heading_base_to_rover;
        diagnostic_fix_type_ = rtk_rel.eGpsNavFixStatus;
    }
}

void InertialSenseROS::GPS_raw_callback(const gps_raw_t *const msg)
{
    switch (msg->dataType)
    {
    case raw_data_type_observation:
        GPS_obs_callback((obsd_t *)&msg->data.obs, msg->obsCount);
        break;

    case raw_data_type_ephemeris:
        GPS_eph_callback((eph_t *)&msg->data.eph);
        break;

    case raw_data_type_glonass_ephemeris:
        GPS_geph_callback((geph_t *)&msg->data.gloEph);
        break;

    default:
        break;
    }
}

void InertialSenseROS::GPS_obs_callback(const obsd_t *const msg, int nObs)
{
    if (obs_Vec_.obs.size() > 0 &&
        (msg[0].time.time != obs_Vec_.obs[0].time.time ||
         msg[0].time.sec != obs_Vec_.obs[0].time.sec))
        GPS_obs_bundle_timer_callback(ros::TimerEvent());

    for (int i = 0; i < nObs; i++)
    {
        inertial_sense_ros::GNSSObservation obs;
        obs.header.stamp = ros_time_from_gtime(msg[i].time.time, msg[i].time.sec);
        obs.time.time = msg[i].time.time;
        obs.time.sec = msg[i].time.sec;
        obs.sat = msg[i].sat;
        obs.rcv = msg[i].rcv;
        obs.SNR = msg[i].SNR[0];
        obs.LLI = msg[i].LLI[0];
        obs.code = msg[i].code[0];
        obs.qualL = msg[i].qualL[0];
        obs.qualP = msg[i].qualP[0];
        obs.L = msg[i].L[0];
        obs.P = msg[i].P[0];
        obs.D = msg[i].D[0];
        obs_Vec_.obs.push_back(obs);
        last_obs_time_ = ros::Time::now();
    }
}

void InertialSenseROS::GPS_obs_bundle_timer_callback(const ros::TimerEvent &e)
{
    if (obs_Vec_.obs.size() == 0)
        return;

    if (abs((ros::Time::now() - last_obs_time_).toSec()) > 1e-2)
    {
        obs_Vec_.header.stamp = ros_time_from_gtime(obs_Vec_.obs[0].time.time, obs_Vec_.obs[0].time.sec);
        obs_Vec_.time = obs_Vec_.obs[0].time;
        GPS_obs_.pub.publish(obs_Vec_);
        obs_Vec_.obs.clear();
        //        cout << "dt" << (obs_Vec_.header.stamp - ros::Time::now()) << endl;
    }
}

void InertialSenseROS::GPS_eph_callback(const eph_t *const msg)
{
    inertial_sense_ros::GNSSEphemeris eph;
    eph.sat = msg->sat;
    eph.iode = msg->iode;
    eph.iodc = msg->iodc;
    eph.sva = msg->sva;
    eph.svh = msg->svh;
    eph.week = msg->week;
    eph.code = msg->code;
    eph.flag = msg->flag;
    eph.toe.time = msg->toe.time;
    eph.toc.time = msg->toc.time;
    eph.ttr.time = msg->ttr.time;
    eph.toe.sec = msg->toe.sec;
    eph.toc.sec = msg->toc.sec;
    eph.ttr.sec = msg->ttr.sec;
    eph.A = msg->A;
    eph.e = msg->e;
    eph.i0 = msg->i0;
    eph.OMG0 = msg->OMG0;
    eph.omg = msg->omg;
    eph.M0 = msg->M0;
    eph.deln = msg->deln;
    eph.OMGd = msg->OMGd;
    eph.idot = msg->idot;
    eph.crc = msg->crc;
    eph.crs = msg->crs;
    eph.cuc = msg->cuc;
    eph.cus = msg->cus;
    eph.cic = msg->cic;
    eph.cis = msg->cis;
    eph.toes = msg->toes;
    eph.fit = msg->fit;
    eph.f0 = msg->f0;
    eph.f1 = msg->f1;
    eph.f2 = msg->f2;
    eph.tgd[0] = msg->tgd[0];
    eph.tgd[1] = msg->tgd[1];
    eph.tgd[2] = msg->tgd[2];
    eph.tgd[3] = msg->tgd[3];
    eph.Adot = msg->Adot;
    eph.ndot = msg->ndot;
    GPS_eph_.pub.publish(eph);
}

void InertialSenseROS::GPS_geph_callback(const geph_t *const msg)
{
    inertial_sense_ros::GlonassEphemeris geph;
    geph.sat = msg->sat;
    geph.iode = msg->iode;
    geph.frq = msg->frq;
    geph.svh = msg->svh;
    geph.sva = msg->sva;
    geph.age = msg->age;
    geph.toe.time = msg->toe.time;
    geph.tof.time = msg->tof.time;
    geph.toe.sec = msg->toe.sec;
    geph.tof.sec = msg->tof.sec;
    geph.pos[0] = msg->pos[0];
    geph.pos[1] = msg->pos[1];
    geph.pos[2] = msg->pos[2];
    geph.vel[0] = msg->vel[0];
    geph.vel[1] = msg->vel[1];
    geph.vel[2] = msg->vel[2];
    geph.acc[0] = msg->acc[0];
    geph.acc[1] = msg->acc[1];
    geph.acc[2] = msg->acc[2];
    geph.taun = msg->taun;
    geph.gamn = msg->gamn;
    geph.dtaun = msg->dtaun;
    GPS_geph_.pub.publish(geph);
}

void InertialSenseROS::diagnostics_callback(const ros::TimerEvent &event)
{
    // Create diagnostic objects
    diagnostic_msgs::DiagnosticArray diag_array;
    diag_array.header.stamp = ros::Time::now();

    // CNO mean
    diagnostic_msgs::DiagnosticStatus cno_mean;
    cno_mean.name = "CNO Mean";
    cno_mean.level = diagnostic_msgs::DiagnosticStatus::OK;
    cno_mean.message = std::to_string(gps_msg.cno);
    diag_array.status.push_back(cno_mean);

    if (RTK_.enabled)
    {
        diagnostic_msgs::DiagnosticStatus rtk_status;
        rtk_status.name = "RTK";
        rtk_status.level = diagnostic_msgs::DiagnosticStatus::OK;
        std::string rtk_message;

        // AR ratio
        diagnostic_msgs::KeyValue ar_ratio;
        ar_ratio.key = "AR Ratio";
        ar_ratio.value = std::to_string(diagnostic_ar_ratio_);
        rtk_status.values.push_back(ar_ratio);
        if (diagnostic_fix_type_ == inertial_sense_ros::RTKRel::GPS_STATUS_FIX_3D)
        {
            rtk_status.level = diagnostic_msgs::DiagnosticStatus::WARN;
            rtk_message = "3D: " + std::to_string(diagnostic_ar_ratio_);
        }
        else if (diagnostic_fix_type_ == inertial_sense_ros::RTKRel::GPS_STATUS_FIX_RTK_SINGLE)
        {
            rtk_status.level = diagnostic_msgs::DiagnosticStatus::WARN;
            rtk_message = "Single: " + std::to_string(diagnostic_ar_ratio_);
        }
        else if (diagnostic_fix_type_ == inertial_sense_ros::RTKRel::GPS_STATUS_FIX_RTK_FLOAT)
        {
            rtk_message = "Float: " + std::to_string(diagnostic_ar_ratio_);
        }
        else if (diagnostic_fix_type_ == inertial_sense_ros::RTKRel::GPS_STATUS_FIX_RTK_FIX)
        {
            rtk_message = "Fix: " + std::to_string(diagnostic_ar_ratio_);
        }
        else if (diagnostic_fix_type_ == inertial_sense_ros::RTKRel::GPS_STATUS_FLAGS_RTK_FIX_AND_HOLD)
        {
            rtk_message = "Fix and Hold: " + std::to_string(diagnostic_ar_ratio_);
        }
        else
        {
            rtk_message = "Unknown Fix: " + std::to_string(diagnostic_ar_ratio_);
        }

        // Differential age
        diagnostic_msgs::KeyValue differential_age;
        differential_age.key = "Differential Age";
        differential_age.value = std::to_string(diagnostic_differential_age_);
        rtk_status.values.push_back(differential_age);
        if (diagnostic_differential_age_ > 1.5)
        {
            rtk_status.level = diagnostic_msgs::DiagnosticStatus::WARN;
            rtk_message += " Differential Age Large";
        }

        // Heading base to rover
        diagnostic_msgs::KeyValue heading_base_to_rover;
        heading_base_to_rover.key = "Heading Base to Rover (rad)";
        heading_base_to_rover.value = std::to_string(diagnostic_heading_base_to_rover_);
        rtk_status.values.push_back(heading_base_to_rover);

        rtk_status.message = rtk_message;
        diag_array.status.push_back(rtk_status);
    }

    diagnostics_.pub.publish(diag_array);
}

bool InertialSenseROS::set_current_position_as_refLLA(std_srvs::Trigger::Request &req, std_srvs::Trigger::Response &res)
{
    (void)req;
    double current_lla_[3];
    current_lla_[0] = lla_[0];
    current_lla_[1] = lla_[1];
    current_lla_[2] = lla_[2];

    IS_.SendData(DID_FLASH_CONFIG, reinterpret_cast<uint8_t *>(&current_lla_), sizeof(current_lla_), offsetof(nvm_flash_cfg_t, refLla));

    comManagerGetData(0, DID_FLASH_CONFIG, 0, 0, 1);

    int i = 0;
    nvm_flash_cfg_t current_flash = IS_.GetFlashConfig();
    while (current_flash.refLla[0] == IS_.GetFlashConfig().refLla[0] && current_flash.refLla[1] == IS_.GetFlashConfig().refLla[1] && current_flash.refLla[2] == IS_.GetFlashConfig().refLla[2])
    {
        comManagerStep();
        i++;
        if (i > 100)
        {
            break;
        }
    }

    if (current_lla_[0] == IS_.GetFlashConfig().refLla[0] && current_lla_[1] == IS_.GetFlashConfig().refLla[1] && current_lla_[2] == IS_.GetFlashConfig().refLla[2])
    {
        comManagerGetData(0, DID_FLASH_CONFIG, 0, 0, 0);
        res.success = true;
        res.message = ("Update was succesful.  refLla: Lat: " + to_string(current_lla_[0]) + "  Lon: " + to_string(current_lla_[1]) + "  Alt: " + to_string(current_lla_[2]));
    }
    else
    {
        comManagerGetData(0, DID_FLASH_CONFIG, 0, 0, 0);
        res.success = false;
        res.message = "Unable to update refLLA. Please try again.";
    }
}

bool InertialSenseROS::set_refLLA_to_value(inertial_sense_ros::refLLAUpdate::Request &req, inertial_sense_ros::refLLAUpdate::Response &res)
{
    IS_.SendData(DID_FLASH_CONFIG, reinterpret_cast<uint8_t *>(&req.lla), sizeof(req.lla), offsetof(nvm_flash_cfg_t, refLla));

    comManagerGetData(0, DID_FLASH_CONFIG, 0, 0, 1);

    int i = 0;
    nvm_flash_cfg_t current_flash = IS_.GetFlashConfig();
    while (current_flash.refLla[0] == IS_.GetFlashConfig().refLla[0] && current_flash.refLla[1] == IS_.GetFlashConfig().refLla[1] && current_flash.refLla[2] == IS_.GetFlashConfig().refLla[2])
    {
        comManagerStep();
        i++;
        if (i > 100)
        {
            break;
        }
    }

    if (req.lla[0] == IS_.GetFlashConfig().refLla[0] && req.lla[1] == IS_.GetFlashConfig().refLla[1] && req.lla[2] == IS_.GetFlashConfig().refLla[2])
    {
        comManagerGetData(0, DID_FLASH_CONFIG, 0, 0, 0);
        res.success = true;
        res.message = ("Update was succesful.  refLla: Lat: " + to_string(req.lla[0]) + "  Lon: " + to_string(req.lla[1]) + "  Alt: " + to_string(req.lla[2]));
    }
    else
    {
        comManagerGetData(0, DID_FLASH_CONFIG, 0, 0, 0);
        res.success = false;
        res.message = "Unable to update refLLA. Please try again.";
    }
}

bool InertialSenseROS::perform_mag_cal_srv_callback(std_srvs::Trigger::Request &req, std_srvs::Trigger::Response &res)
{
    (void)req;
    uint32_t single_axis_command = 2;
    IS_.SendData(DID_MAG_CAL, reinterpret_cast<uint8_t *>(&single_axis_command), sizeof(uint32_t), offsetof(mag_cal_t, recalCmd));

    is_comm_instance_t comm;
    uint8_t buffer[2048];
    is_comm_init(&comm, buffer, sizeof(buffer));
    serial_port_t *serialPort = IS_.GetSerialPort();
    uint8_t inByte;
    int n;

    while ((n = serialPortReadCharTimeout(serialPort, &inByte, 20)) > 0)
    {
        // Search comm buffer for valid packets
        if (is_comm_parse_byte(&comm, inByte) == _PTYPE_INERTIAL_SENSE_DATA && comm.dataHdr.id == DID_INS_1)
        {
            ins_1_t *msg = (ins_1_t *)(comm.dataPtr + comm.dataHdr.offset);
            if (msg->insStatus & 0x00400000)
            {
                res.success = true;
                res.message = "Successfully initiated mag recalibration.";
                return true;
            }
        }
    }
}

bool InertialSenseROS::perform_multi_mag_cal_srv_callback(std_srvs::Trigger::Request &req, std_srvs::Trigger::Response &res)
{
    (void)req;
    uint32_t multi_axis_command = 1;
    IS_.SendData(DID_MAG_CAL, reinterpret_cast<uint8_t *>(&multi_axis_command), sizeof(uint32_t), offsetof(mag_cal_t, recalCmd));

    is_comm_instance_t comm;
    uint8_t buffer[2048];
    is_comm_init(&comm, buffer, sizeof(buffer));
    serial_port_t *serialPort = IS_.GetSerialPort();
    uint8_t inByte;
    int n;

    while ((n = serialPortReadCharTimeout(serialPort, &inByte, 20)) > 0)
    {
        // Search comm buffer for valid packets
        if (is_comm_parse_byte(&comm, inByte) == _PTYPE_INERTIAL_SENSE_DATA && comm.dataHdr.id == DID_INS_1)
        {
            ins_1_t *msg = (ins_1_t *)(comm.dataPtr + comm.dataHdr.offset);
            if (msg->insStatus & 0x00400000)
            {
                res.success = true;
                res.message = "Successfully initiated mag recalibration.";
                return true;
            }
        }
    }
}

void InertialSenseROS::reset_device()
{
    // send reset command
    system_command_t reset_command;
    reset_command.command = 99;
    reset_command.invCommand = ~reset_command.command;
    IS_.SendData(DID_SYS_CMD, reinterpret_cast<uint8_t *>(&reset_command), sizeof(system_command_t), 0);
    sleep(1);
}

bool InertialSenseROS::update_firmware_srv_callback(inertial_sense_ros::FirmwareUpdate::Request &req, inertial_sense_ros::FirmwareUpdate::Response &res)
{
    //   IS_.Close();
    //   vector<InertialSense::bootload_result_t> results = IS_.BootloadFile("*", req.filename, 921600);
    //   if (!results[0].error.empty())
    //   {
    //     res.success = false;
    //     res.message = results[0].error;
    //     return false;
    //   }
    //   IS_.Open(port_.c_str(), baudrate_);
    //   return true;
}

ros::Time InertialSenseROS::ros_time_from_week_and_tow(const uint32_t week, const double timeOfWeek)
{
    ros::Time rostime(0, 0);
    //  If we have a GPS fix, then use it to set timestamp
    if (abs(GPS_towOffset_) > 0.001)
    {
        uint64_t sec = UNIX_TO_GPS_OFFSET + floor(timeOfWeek) + week * 7 * 24 * 3600;
        uint64_t nsec = (timeOfWeek - floor(timeOfWeek)) * 1e9;
        rostime = ros::Time(sec, nsec);
    }
    else
    {
        // Otherwise, estimate the uINS boot time and offset the messages
        if (!got_first_message_)
        {
            got_first_message_ = true;
            INS_local_offset_ = ros::Time::now().toSec() - timeOfWeek;
        }
        else // low-pass filter offset to account for drift
        {
            double y_offset = ros::Time::now().toSec() - timeOfWeek;
            INS_local_offset_ = 0.005 * y_offset + 0.995 * INS_local_offset_;
        }
        // Publish with ROS time
        rostime = ros::Time(INS_local_offset_ + timeOfWeek);
    }
    return rostime;
}

ros::Time InertialSenseROS::ros_time_from_start_time(const double time)
{
    ros::Time rostime(0, 0);

    //  If we have a GPS fix, then use it to set timestamp
    if (abs(GPS_towOffset_) > 0.001)
    {
        double timeOfWeek = time + GPS_towOffset_;
        uint64_t sec = (uint64_t)(UNIX_TO_GPS_OFFSET + floor(timeOfWeek) + GPS_week_ * 7 * 24 * 3600);
        uint64_t nsec = (uint64_t)((timeOfWeek - floor(timeOfWeek)) * 1.0e9);
        rostime = ros::Time(sec, nsec);
    }
    else
    {
        // Otherwise, estimate the uINS boot time and offset the messages
        if (!got_first_message_)
        {
            got_first_message_ = true;
            INS_local_offset_ = ros::Time::now().toSec() - time;
        }
        else // low-pass filter offset to account for drift
        {
            double y_offset = ros::Time::now().toSec() - time;
            INS_local_offset_ = 0.005 * y_offset + 0.995 * INS_local_offset_;
        }
        // Publish with ROS time
        rostime = ros::Time(INS_local_offset_ + time);
    }
    return rostime;
}

ros::Time InertialSenseROS::ros_time_from_tow(const double tow)
{
    return ros_time_from_week_and_tow(GPS_week_, tow);
}

double InertialSenseROS::tow_from_ros_time(const ros::Time &rt)
{
    return (rt.sec - UNIX_TO_GPS_OFFSET - GPS_week_ * 604800) + rt.nsec * 1.0e-9;
}

ros::Time InertialSenseROS::ros_time_from_gtime(const uint64_t sec, double subsec)
{
    ros::Time out;
    out.sec = sec - LEAP_SECONDS;
    out.nsec = subsec * 1e9;
    return out;
}


void InertialSenseROS::LD2Cov(const float *LD, float *Cov, int width)
{
    for (int j = 0; j < width; j++) {
        for (int i = 0; i < width; i++) {
            if (i < j) {
                Cov[i * width + j] = Cov[j * width + i];
            }
            else {
                Cov[i * width + j] = LD[(i * i + i) / 2 + j];
            }
        }
    }
}

void InertialSenseROS::rotMatB2R(const ixVector4 quat, ixMatrix3 R)
{
    R[0] = 1.0f - 2.0f * (quat[2]*quat[2] + quat[3]*quat[3]);
    R[1] =        2.0f * (quat[1]*quat[2] - quat[0]*quat[3]);
    R[2] =        2.0f * (quat[1]*quat[3] + quat[0]*quat[2]);
    R[3] =        2.0f * (quat[1]*quat[2] + quat[0]*quat[3]);
    R[4] = 1.0f - 2.0f * (quat[1]*quat[1] + quat[3]*quat[3]);
    R[5] =        2.0f * (quat[2]*quat[3] - quat[0]*quat[1]);
    R[6] =        2.0f * (quat[1]*quat[3] - quat[0]*quat[2]);
    R[7] =        2.0f * (quat[2]*quat[3] + quat[0]*quat[1]);
    R[8] = 1.0f - 2.0f * (quat[1]*quat[1] + quat[2]*quat[2]);
}

void InertialSenseROS::transform_6x6_covariance(float Pout[36], float Pin[36], ixMatrix3 R1, ixMatrix3 R2)
{
    // Assumption: input covariance matrix is transformed due to change of coordinates, 
    // so that fisrt 3 coordinates are rotated by R1 and the last 3 coordinates are rotated by R2
    // This is how the transformation looks:
    // |R1  0 | * |Pxx  Pxy'| * |R1' 0  | = |R1*Pxx*R1'  R1*Pxy'*R2'|
    // |0   R2|   |Pxy  Pyy |   |0   R2'|   |R2*Pxy*R1'  R2*Pyy*R2' |

    ixMatrix3 Pxx_in, Pxy_in, Pyy_in, Pxx_out, Pxy_out, Pyy_out, buf;

    // Extract 3x3 blocks from input covariance
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            // Upper diagonal block in old frame
            Pxx_in[i * 3 + j] = Pin[i * 6 + j];
            // Lower left block of in old frame
            Pxy_in[i * 3 + j] = Pin[(i+3) * 6 + j];
            // Lower diagonal block in old frame
            Pyy_in[i * 3 + j] = Pin[(i+3) * 6 + j + 3];
        }
    }
    // Transform the 3x3 covariance blocks
    // New upper diagonal block
    mul_Mat3x3_Mat3x3(buf, R1, Pxx_in);
    mul_Mat3x3_Mat3x3_Trans(Pxx_out, buf, R1);
    // New lower left block
    mul_Mat3x3_Mat3x3(buf, R2, Pxy_in);
    mul_Mat3x3_Mat3x3_Trans(Pxy_out, buf, R1);
    // New lower diagonal  block
    mul_Mat3x3_Mat3x3(buf, R2, Pyy_in);
    mul_Mat3x3_Mat3x3_Trans(Pyy_out, buf, R2);

    // Copy the computed transformed blocks into output 6x6 covariance matrix
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            // Upper diagonal block in the new frame
            Pout[i * 6 + j] = Pxx_in[i * 3 + j];
            // Lower left block in the new frame
            Pout[(i+3) * 6 + j] = Pxy_in[i * 3 + j];
            // Upper right block in the new frame
            Pout[i * 6 + j + 3] = Pxy_in[j * 3 + i];
            // Lower diagonal block in the new frame
            Pout[(i+3) * 6 + j + 3] = Pyy_in[i * 3 + j];
        }
    }
}
