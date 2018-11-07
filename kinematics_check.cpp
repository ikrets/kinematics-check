//
// Copyright (c) 2018, Can Erdogan & Arne Sievelring
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// * Redistributions of source code must retain the above copyright notice,
//   this list of conditions and the following disclaimer.
// * Redistributions in binary form must reproduce the above copyright notice,
//   this list of conditions and the following disclaimer in the documentation
//   and/or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.
//

#include <QApplication>
#include <QMetaType>
#include <QThread>
#include <stdexcept>
#include <random>
#include <rl/math/Transform.h>
#include <rl/math/Vector.h>
#include <rl/math/Rotation.h>
#include <rl/plan/VectorList.h>

#include "ros/ros.h"
#include <ros/package.h>
#include <eigen_conversions/eigen_msg.h>
#include "kinematics_check/CheckKinematics.h"

#include "MainWindow.h"

MainWindow* MainWindow::singleton = NULL;

class ROSThread : public QThread
{
public:
    void run(){

      //Set the update rate at which the interface receives motion commands
      ros::Rate loop_rate = 20;

      while (ros::ok())
      {
        ros::spinOnce();
        //std::cout<<"spin"<<std::endl;

        loop_rate.sleep();
      }
  }
};

bool query(kinematics_check::CheckKinematics::Request  &req,
         kinematics_check::CheckKinematics::Response &res)
{

  ROS_INFO("Recieving query");
  MainWindow* mw = MainWindow::instance();
  mw->start = boost::make_shared< rl::math::Vector >(req.initial_configuration.size());

  for(int i=0; i<req.initial_configuration.size(); i++)
  {
    (*mw->start)(i) = req.initial_configuration[i];
  }

  auto convertToStdVector = [](const rl::math::Vector& config) {
    return std::vector<rl::math::Real>(config.data(), config.data() + config.size());
  };

  // Create a frame from the position/quaternion data
  Eigen::Affine3d ifco_transform;
  Eigen::Affine3d goal_transform;
  tf::poseMsgToEigen(req.ifco_pose, ifco_transform);
  tf::poseMsgToEigen(req.goal_pose, goal_transform);

  mw->goalFrame = boost::make_shared<rl::math::Transform>(goal_transform);
  mw->desiredCollObj = req.allowed_collision_object;

  ROS_INFO("Trying to plan to the goal frame");
  mw->plan(ifco_transform, req.bounding_boxes_with_poses);

  if (mw->lastPlanningResult)
  {
    ROS_INFO("Reached the exact goal frame");
    res.success = true;
    res.final_configuration = convertToStdVector(mw->lastTrajectory[mw->lastTrajectory.size()-1]);
    return true;
  }

  std::array<std::uniform_real_distribution<double>, 3> coordinate_distributions = {
    std::uniform_real_distribution<double>(-req.position_deltas[0], req.position_deltas[0]),
    std::uniform_real_distribution<double>(-req.position_deltas[1], req.position_deltas[1]),
    std::uniform_real_distribution<double>(-req.position_deltas[2], req.position_deltas[2])
  };

  std::array<std::uniform_real_distribution<double>, 3> angle_distributions = {
    std::uniform_real_distribution<double>(-req.orientation_deltas[0], req.orientation_deltas[0]),
    std::uniform_real_distribution<double>(-req.orientation_deltas[1], req.orientation_deltas[1]),
    std::uniform_real_distribution<double>(-req.orientation_deltas[2], req.orientation_deltas[2])
  };

  mw->resetViewerBoxes();
  mw->drawBox(rl::math::Vector3(2 * req.position_deltas[0], 2 * req.position_deltas[1], 2 * req.position_deltas[2]),
              *mw->goalFrame);
  std::mt19937 generator(time(nullptr));

  int sample_count;
  ros::NodeHandle n;
  n.param("sample_count", sample_count, 20);

  ROS_INFO("Beginning to sample within acceptable deltas");
  for (unsigned i = 0; i < sample_count; ++i)
  {
    rl::math::Vector3 sampled_point;
    std::array<double, 3> sampled_rotation;

    for (unsigned i = 0; i < 3; ++i)
    {
      sampled_point(i) = coordinate_distributions[i](generator);
      sampled_rotation[i] = angle_distributions[i](generator);
    }

    // goal frame orientation does not change
    mw->goalFrame = boost::make_shared<rl::math::Transform>(goal_transform);
    mw->goalFrame->translation() += sampled_point;

    mw->goalFrame->linear() = rl::math::AngleAxis(sampled_rotation[2], rl::math::Vector3::UnitZ()) *
                              rl::math::AngleAxis(sampled_rotation[1], rl::math::Vector3::UnitY()) *
                              rl::math::AngleAxis(sampled_rotation[0], rl::math::Vector3::UnitX()) *
                              goal_transform.linear();

    ROS_INFO_STREAM("Trying goal frame: " << mw->goalFrame->translation() << ", " << mw->goalFrame->rotation());
    mw->plan(ifco_transform, req.bounding_boxes_with_poses);

    if (mw->lastPlanningResult)
    {
      ROS_INFO("Reached the goal frame in attempt %d", i + 1);
      res.success = true;
      res.final_configuration = convertToStdVector(mw->lastTrajectory[mw->lastTrajectory.size() - 1]);
      return true;
    }
  }

  ROS_INFO("Could not reach the goal frame with deltas after %d attempts", sample_count);
  return true;
}


int
main(int argc, char** argv)
{

	try
	{

    ros::init(argc, argv, "check_kinematics_server");
    ros::NodeHandle n;

    ros::ServiceServer service = n.advertiseService("check_kinematics", query);


		QApplication application(argc, argv);
		
		qRegisterMetaType< rl::math::Real >("rl::math::Real");
		qRegisterMetaType< rl::math::Transform >("rl::math::Transform");
		qRegisterMetaType< rl::math::Vector >("rl::math::Vector");
		qRegisterMetaType< rl::plan::VectorList >("rl::plan::VectorList");
		qRegisterMetaType< SbColor >("SbColor");
		
		QObject::connect(&application, SIGNAL(lastWindowClosed()), &application, SLOT(quit()));


    bool hide_window;
    n.param("hide_window", hide_window, false);

    if (hide_window)
      MainWindow::instance()->hide();
    else
      MainWindow::instance()->show();

    ROSThread ros_thread;
    ros_thread.start();

		return application.exec();
	}
	catch (const std::exception& e)
	{
		std::cerr << e.what() << std::endl;
		return -1;
	}
}
