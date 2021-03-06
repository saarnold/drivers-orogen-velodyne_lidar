/* Generated from orogen/lib/orogen/templates/tasks/Task.cpp */

#include "Positioning.hpp"

#include <rtt/extras/FileDescriptorActivity.hpp>
#include <aggregator/TimestampEstimator.hpp>
#include <ogr_spatialref.h>
#include <base/float.h>

using namespace velodyne_lidar;

Positioning::Positioning(std::string const& name)
    : PositioningBase(name)
{
    orientation_output_ports[0] = &_orientation_sensors_1;
    orientation_output_ports[1] = &_orientation_sensors_2;
    orientation_output_ports[2] = &_orientation_sensors_3;
}

Positioning::Positioning(std::string const& name, RTT::ExecutionEngine* engine)
    : PositioningBase(name, engine)
{
    orientation_output_ports[0] = &_orientation_sensors_1;
    orientation_output_ports[1] = &_orientation_sensors_2;
    orientation_output_ports[2] = &_orientation_sensors_3;
}

Positioning::~Positioning()
{
}



/// The following lines are template definitions for the various state machine
// hooks defined by Orocos::RTT. See Positioning.hpp for more detailed
// documentation about them.




bool Positioning::configureHook()
{
    if (! RTT::TaskContext::configureHook())
        return false;

    // setup conversion from WGS84 to UTM
    OGRSpatialReference oSourceSRS;
    OGRSpatialReference oTargetSRS;
    
    oSourceSRS.SetWellKnownGeogCS( "WGS84" );
    oTargetSRS.SetWellKnownGeogCS( "WGS84" );
    oTargetSRS.SetUTM( _utm_zone, _utm_north );

    coTransform = OGRCreateCoordinateTransformation(&oSourceSRS, &oTargetSRS);

    if( coTransform == NULL )
    {
        RTT::log(RTT::Error) << "failed to initialize CoordinateTransform UTM_ZONE:" << _utm_zone << " UTM_NORTH:" << _utm_north << RTT::endlog();
        return false;
    }
    
    timestamp_estimator = new aggregator::TimestampEstimator(base::Time::fromSeconds(20), base::Time::fromMilliseconds(100));
    
    return true;
}



bool Positioning::startHook()
{
    if (! RTT::TaskContext::startHook())
        return false;
    
    // setup udp server
    positioning_driver.openUDP("", VELODYNE_POSITIONING_UDP_PORT);
    
    // trigger the update hook on fd activity
    RTT::extras::FileDescriptorActivity* fd_activity =
        getActivity<RTT::extras::FileDescriptorActivity>();
    if (fd_activity)
    {
        fd_activity->watch(positioning_driver.getFileDescriptor());
    }
    else
    {
        RTT::log(RTT::Error) << TaskContext::getName() << ": "
                    << "Error: the task needs to be fd driven." << RTT::endlog();
        return false;
    }
    
    last_state = PRE_OPERATIONAL;
    last_packet_period = 1000000;
    last_gps_timestamp = 0;
    gps_timestamp_tolerance = 100;
    
    return true;
}



void Positioning::updateHook()
{
    RTT::TaskContext::updateHook();
    
    States actual_state = RUNNING;
    
    base::Time timeout = base::Time::fromMilliseconds(_timeout.get());
    int size = 0;
    try 
    {
        size = positioning_driver.readPacket((uint8_t*)&buffer, VELODYNE_POSITIONING_MSG_BUFFER_SIZE, timeout, timeout);
    }
    catch (std::runtime_error e)
    {
        RTT::log(RTT::Error) << TaskContext::getName() << ": " << e.what() << RTT::endlog();
        actual_state = IO_TIMEOUT;
    }
    
    if(size == (int)VELODYNE_POSITIONING_MSG_BUFFER_SIZE)
    {
        // check packet timestamp
        if(last_gps_timestamp > 0 && last_gps_timestamp < buffer.gps_timestamp)
        {
            if(last_packet_period + gps_timestamp_tolerance < buffer.gps_timestamp - last_gps_timestamp)
            {
                // handle packet lost
                RTT::log(RTT::Info) << TaskContext::getName() << ": "
                    << "Lost at least one positioning packet." << RTT::endlog();
            }
            last_packet_period = buffer.gps_timestamp - last_gps_timestamp;
        }
        last_gps_timestamp = buffer.gps_timestamp;
        
        try 
        {
            // handle gps data
            GPS_RMC gps_data;
            positioning_driver.convertNMEASentence(buffer.nmea_sentence, gps_data);
            _gps_raw_data.write(gps_data);
            // get local utc time zone
            time_t local_time;
            time(&local_time);
            struct tm* local_time_info = localtime(&local_time);
            // estimate timestamp in local time
            base::Time timestamp = timestamp_estimator->update(gps_data.utc_time + base::Time::fromSeconds((int64_t)local_time_info->tm_gmtoff));
            
            // if there is a valid reading, then write it to position readings port
            if(gps_data.status == ValidPosition && gps_data.signal_mode != InvalidMode)
            { 
                double la = (gps_data.latitude_hemisphere == North ? gps_data.latitude : gps_data.latitude * -1.0);
                double lo = (gps_data.longitude_hemisphere == East ? gps_data.longitude : gps_data.longitude * -1.0);
                double alt = 0.0;

                coTransform->Transform(1, &lo, &la, &alt);
                base::samples::RigidBodyState pos;
                pos.time = timestamp;
                pos.position.x() = lo - _origin.value().x();
                pos.position.y() = la - _origin.value().y();
                pos.position.z() = base::unknown<double>();
                
                base::Vector3d angular_velocity = Eigen::AngleAxisd(base::Angle::fromDeg(gps_data.angle).getRad(), base::Vector3d::UnitZ()) * base::Vector3d::UnitX();
                pos.angular_velocity = angular_velocity * (gps_data.speed * 0.51444); // knots to m/s
                
                _position_samples.write(pos);
            }
            
            // handle motion values
            base::samples::IMUSensors imu_data;
            imu_data.time = timestamp;
            // invalidate values
            imu_data.acc = base::Vector3d::Ones() * base::NaN<double>();
            imu_data.gyro = base::Vector3d::Ones() * base::NaN<double>();
            imu_data.mag = base::Vector3d::Ones() * base::NaN<double>();
            // write the raw orientation readings to output ports
            for(unsigned i = 0; i < VELODYNE_ORIENTATION_READINGS; i++)
            {
                double gyro, temp, accel_x, accel_y;
                positioning_driver.convertMotionValues(buffer.orientations[i], gyro, temp, accel_x, accel_y);
                
                imu_data.gyro.x() = gyro;
                imu_data.acc.x() = accel_x;
                imu_data.acc.y() = accel_y;
                
                orientation_output_ports[i]->write(imu_data);
            }
            
        }
        catch (std::runtime_error e)
        {
            RTT::log(RTT::Error) << TaskContext::getName() << ": " << e.what() << RTT::endlog();
            actual_state = IO_ERROR;
        }
        
    }
    else
    {
        RTT::log(RTT::Error) << TaskContext::getName() << ": "
            << "Received an unknown packet of size " << size 
            << ". The velodyne positioning message should be of size " 
            << VELODYNE_POSITIONING_MSG_BUFFER_SIZE << RTT::endlog();
        actual_state = IO_ERROR;
    }
    
    
    
    // write state if it has changed
    if(last_state != actual_state)
    {
        last_state = actual_state;
        state(actual_state);
    }
    
}



void Positioning::errorHook()
{
    
    RTT::TaskContext::errorHook();
    

    

    
}



void Positioning::stopHook()
{
    
    RTT::TaskContext::stopHook();
    

    positioning_driver.close();

    
}



void Positioning::cleanupHook()
{
    
    RTT::TaskContext::cleanupHook();
    

    delete timestamp_estimator;

    
}

