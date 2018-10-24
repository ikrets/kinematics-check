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
#include <rl/plan/VectorList.h>

#include "ros/ros.h"
#include <ros/package.h>
#include "kinematics_check/CheckKinematics.h"

#include "MainWindow.h"

MainWindow* MainWindow::singleton = NULL;


class ROSThread : public QThread
{
public:
    void run(){

      //Set the update rate at which the interface receives motion commands
      ros::Rate loopRate = 20;

      while (ros::ok())
      {
        ros::spinOnce();
        //std::cout<<"spin"<<std::endl;

        loopRate.sleep();
      }
  }
};

bool query(kinematics_check::CheckKinematics::Request  &req,
         kinematics_check::CheckKinematics::Response &res)
{

  ROS_INFO("Recieving query");
  MainWindow* mw = MainWindow::instance();
  mw->start = boost::make_shared< rl::math::Vector >(req.joints.size());

  for(int i=0; i<req.joints.size(); i++)
  {
    (*mw->start)(i) = req.joints[i];
  }

  auto makeTransformFromPose = [](const geometry_msgs::Pose& pose)
  {
    auto transform = boost::make_shared<rl::math::Transform>();
    Eigen::Quaternion<double> q(pose.orientation.w, pose.orientation.x, pose.orientation.y,
                                pose.orientation.z);
    // TODO assign the quaternion directly
    transform->linear() = q.toRotationMatrix();
    transform->translation() = Eigen::Vector3d(pose.position.x, pose.position.y, pose.position.z);
    return transform;
  };

  auto configToJoints = [](const rl::math::Vector& config)
  {
    kinematics_check::CheckKinematics::Response::_finalJoints_type finalJoints;
    finalJoints.resize(config.size());
    for (int i = 0; i < config.size(); i++)
    {
      finalJoints[i] = config(i);
    }

    return finalJoints;
  };

  // Create a frame from the position/quaternion data
  mw->goalFrame = makeTransformFromPose(req.goalFrame);
  mw->desiredCollObj = req.collObject;

  ROS_INFO("Trying to plan to the goal frame");
  mw->plan();

  if (mw->lastPlanningResult)
  {
    ROS_INFO("Reached the exact goal frame");
    res.success = true;
    res.finalJoints = configToJoints(mw->lastTrajectory[mw->lastTrajectory.size()-1]);
    return true;
  }

  std::array<std::uniform_real_distribution<double>, 3> coordinatesDistributions = {
    std::uniform_real_distribution<double>(-req.boxSize[0] / 2, req.boxSize[0] / 2),
    std::uniform_real_distribution<double>(-req.boxSize[1] / 2, req.boxSize[1] / 2),
    std::uniform_real_distribution<double>(-req.boxSize[2] / 2, req.boxSize[2] / 2)
  };
  rl::math::Quaternion boxOrientation(req.boxOrientation.w, req.boxOrientation.x, req.boxOrientation.y,
                                      req.boxOrientation.z);
  mw->resetViewerBoxes();
  auto boxTransform = *mw->goalFrame;
  boxTransform.linear() = boxOrientation.toRotationMatrix();
  mw->drawBox(rl::math::Vector3(req.boxSize[0], req.boxSize[1], req.boxSize[2]), boxTransform);
  std::mt19937 generator(time(nullptr));

  ROS_INFO("Beginning to sample within acceptable deltas");
  for (unsigned i = 0; i < req.sampleAttempts; ++i)
  {
    rl::math::Vector3 sampledPoint;
    for (unsigned i = 0; i < 3; ++i)
      sampledPoint(i) = coordinatesDistributions[i](generator);
    sampledPoint = boxTransform * sampledPoint;

    // goal frame orientation does not change
    mw->goalFrame->translation() = sampledPoint;
    mw->plan();

    if (mw->lastPlanningResult)
    {
      ROS_INFO("Reached the goal frame in attempt %d", i + 1);
      res.success = true;
      res.finalJoints = configToJoints(mw->lastTrajectory[mw->lastTrajectory.size() - 1]);
      return true;
    }
  }

  ROS_INFO("Could not reach the goal frame with deltas after %d attempts", req.sampleAttempts);
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


    // Decide if the main window should be hidden based on the input parameter '--hide'
    if(argc > 1) {
      bool foundHide = false;
      for(int i = 1; i < argc; i++) {
        if(strcmp(argv[i], "--hide") == 0) {
          foundHide = true;
          break;
        }
      }
      if(foundHide)
        MainWindow::instance()->hide();
      else
        MainWindow::instance()->show();
    }
    else
      MainWindow::instance()->show();

    ROSThread rosThread;
    rosThread.start();

		return application.exec();
	}
	catch (const std::exception& e)
	{
		std::cerr << e.what() << std::endl;
		return -1;
	}
}
