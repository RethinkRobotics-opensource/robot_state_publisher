# Mutable Robot State Publisher
Allows you to publish the state of a robot (i.e the position of its base and all joints) via the "tf" transform library
This fork of the standard ROS robot_state_publisher adds the ability to dynamically change the URDF at runtime,
by adding joint and linkage trees. Hotswapping end-effectors on a robot's arm is now possible without needing to restart the robot_state_publisher.
