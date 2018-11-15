#!/bin/bash

# This script does two successful checks that do not trigger contact manifold
# sampling. The bounding box of the grasped object is colored green.
# All objects that are not explicitly listed are forbidden collisions.

# A terminating collision is an object that can be touched by the sensorized
# parts of the robot. A collision with such an object terminates planning
# and success is set to true.

# A ignored collision is an object that can be touched by the sensorized
# parts of the robot. Such a collision is ignored by the planning algorithm.

# In the first check, the end effector goes down to the pregrasp position.
# Bottom of the ifco is a terminating collision.
rosservice call /check_kinematics "
initial_configuration: [0.1, 0.1, 0, 2.3, 0, 0.5, 0]
goal_pose:
  position: {x: 0.45, y: 0.10, z: 0.35}
  orientation: {x: 0.6830127, y: -0.6830127, z: 0.1830127, w: 0.1830127}
ifco_pose:
  position: {x: -0.12, y: 0, z: 0.1}
  orientation: {x: 0.0, y: 0.0, z: 0.0, w: 1}
bounding_boxes_with_poses:
- box:
    type: 0
    dimensions: [0.09, 0.09, 0.09]
  pose:
    position: {x: 0.45, y: -0.02, z: 0.3}
    orientation: {x: 0.0, y: 0.0, z: 0.0, w: 1.0}
- box:
    type: 0
    dimensions: [0.08, 0.08, 0.08]
  pose:
    position: {x: 0.6, y: -0.2, z: 0.3}
    orientation: {x: 0.0, y: 0.0, z: 0.0, w: 1.0}
position_deltas: [0.05, 0.05, 0.05]
orientation_deltas: [0, 0, 0]
allowed_collisions:
- {type: 2, constraint_name: 'bottom', terminate_on_collision: true}
" 

# The second check starts from the final configuration of the first check.
# The end effector simulates the sliding motion.
# Grasped object and bottom of the ifco are ignored collisions.
# South wall of the ifco is terminating collision.
rosservice call /check_kinematics "
initial_configuration: [0.3915, 0.5990, -0.0230, 2.2690, -0.7984, 0.6347, -0.3858]
goal_pose:
  position: {x: 0.45, y: -0.4, z: 0.35}
  orientation: {x: 0.6830127, y: -0.6830127, z: 0.1830127, w: 0.1830127}
ifco_pose:
  position: {x: -0.12, y: 0, z: 0.1}
  orientation: {x: 0.0, y: 0.0, z: 0.0, w: 1}
bounding_boxes_with_poses:
- box:
    type: 0
    dimensions: [0.09, 0.09, 0.09]
  pose:
    position: {x: 0.45, y: -0.02, z: 0.3}
    orientation: {x: 0.0, y: 0.0, z: 0.0, w: 1.0}
- box:
    type: 0
    dimensions: [0.08, 0.08, 0.08]
  pose:
    position: {x: 0.6, y: -0.2, z: 0.3}
    orientation: {x: 0.0, y: 0.0, z: 0.0, w: 1.0}
position_deltas: [0.05, 0.05, 0.05]
orientation_deltas: [0, 0, 0]
allowed_collisions:
- {type: 2, constraint_name: 'bottom', terminate_on_collision: false}
- {type: 1, box_id: 0, terminate_on_collision: false}
- {type: 2, constraint_name: 'south', terminate_on_collision: true}
" 