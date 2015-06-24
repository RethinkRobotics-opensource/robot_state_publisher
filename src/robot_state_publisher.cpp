/*********************************************************************
 * Software License Agreement (BSD License)
 *
 *  Copyright (c) 2008, Willow Garage, Inc.
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions
 *  are met:
 *
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above
 *     copyright notice, this list of conditions and the following
 *     disclaimer in the documentation and/or other materials provided
 *     with the distribution.
 *   * Neither the name of the Willow Garage nor the names of its
 *     contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 *  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 *  COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 *  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 *  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 *  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 *  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 *  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 *  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 *********************************************************************/

/* Author: Wim Meeussen */

#include <kdl/frames_io.hpp>
#include <geometry_msgs/TransformStamped.h>
#include <tf2_kdl/tf2_kdl.h>
#include <memory>
#include "robot_state_publisher/robot_state_publisher.h"

using namespace std;
using namespace ros;

namespace robot_state_publisher {

RobotStatePublisher::RobotStatePublisher(const urdf::Model& model)
    : initialized_(false), model_(model)
{
  setJointMimicMap(model);
}


  bool RobotStatePublisher::init()
  {
    if (RobotKDLTree::init())
    {
      // walk the tree and add segments to segments_
      addChildren(getTree().getRootSegment());
      initialized_ = true;
    }

    if (!initialized_)  ROS_ERROR("robot_state_publisher:  failed to initialize!");
    return initialized_;
  }

  void RobotStatePublisher::setJointMimicMap(const urdf::Model& model)
  {
    ROS_DEBUG("robot_state_publisher: Updating MimicMap.");
    // get exclusive access for writing
    boost::unique_lock<boost::shared_mutex> lock(mimic_mtx_);
    mimic_.clear();
    for(std::map< std::string, std::shared_ptr< urdf::Joint > >::const_iterator i = model.joints_.begin(); i != model.joints_.end(); i++){
      if(i->second->mimic){
        mimic_.insert(make_pair(i->first, i->second->mimic));
      }
    }
  }

  bool RobotStatePublisher::getJointMimicPositions(std::map<std::string, double>& joint_positions)
  {
    // get shared access for reading
    boost::shared_lock<boost::shared_mutex> lock(mimic_mtx_, boost::try_to_lock);
    if (!lock.owns_lock())
    {
      ROS_DEBUG("robot_state_publisher: Failed to update positions for Mimic joints -- could not get lock");
      return false;
    }
    for(MimicMap::const_iterator i = mimic_.begin(); i != mimic_.end(); i++){
      if(joint_positions.find(i->second->joint_name) != joint_positions.end()){
        double pos = joint_positions[i->second->joint_name] * i->second->multiplier + i->second->offset;
        joint_positions.insert(make_pair(i->first, pos));
      }
    }
    return true;
  }

  /** This is called whenever a segment changes.
   *  When that happens, rebuild all of the tf segments.
   *  For efficiency it should be possible to find and rebuild only
   *  the relevant segment, but in practice the URDF doesn't change
   *  very often.
   */
  void RobotStatePublisher::onURDFSwap(const std::string &link_name)
  {
    if (!initialized_)  return;

    RobotKDLTree::onURDFSwap(link_name);

    // Regenerate the segments:
    segments_fixed_.clear();
    segments_.clear();
    // walk the tree and add segments to segments_
    addChildren(getTree().getRootSegment());
    boost::shared_ptr<const urdf::Model> urdf_ptr = getUrdfPtr();
    if(urdf_ptr){
      setJointMimicMap(*urdf_ptr);
    }
    else{
      ROS_ERROR("robot_state_publisher: failed retrieve Robot Model for updating joint MimicMap!");
    }
    urdf_changed_ = true;
  }

  void RobotStatePublisher::setRobotDescriptionIfChanged()
  {
    if (urdf_changed_)
    {
      urdf_changed_ = false;
      setRobotDescription();
      publishFixedTransforms(true); // TODO: only publish if static transforms were used before
    }
  }

// add children to correct maps
void RobotStatePublisher::addChildren(const KDL::SegmentMap::const_iterator segment)
{
  const std::string& root = GetTreeElementSegment(segment->second).getName();

  const std::vector<KDL::SegmentMap::const_iterator>& children = GetTreeElementChildren(segment->second);
  for (unsigned int i=0; i<children.size(); i++) {
    const KDL::Segment& child = GetTreeElementSegment(children[i]->second);
    SegmentPair s(GetTreeElementSegment(children[i]->second), root, child.getName());
    if (child.getJoint().getType() == KDL::Joint::None) {
      if (model_.getJoint(child.getJoint().getName()) && model_.getJoint(child.getJoint().getName())->type == urdf::Joint::FLOATING) {
        ROS_INFO("Floating joint. Not adding segment from %s to %s. This TF can not be published based on joint_states info", root.c_str(), child.getName().c_str());
      }
      else {
        segments_fixed_.insert(make_pair(child.getJoint().getName(), s));
        ROS_DEBUG("Adding fixed segment from %s to %s", root.c_str(), child.getName().c_str());
      }
    }
    else {
      segments_.insert(make_pair(child.getJoint().getName(), s));
      ROS_DEBUG("Adding moving segment from %s to %s", root.c_str(), child.getName().c_str());
    }
    addChildren(children[i]);
  }
}

std::string stripSlash(const std::string & in)
{
  if (in.size() && in[0] == '/')
  {
    return in.substr(1);
  }
  return in;
}

// publish moving transforms
void RobotStatePublisher::publishTransforms(const map<string, double>& joint_positions, const Time& time)
{
  boost::unique_lock<boost::shared_mutex> lock(m_swapMutex, boost::try_to_lock);
  if (!lock.owns_lock())
  {
    ROS_DEBUG("Publishing transforms for moving joints -- could not get lock");
    return;
  }
  ROS_DEBUG("Publishing transforms for moving joints");
  std::vector<geometry_msgs::TransformStamped> tf_transforms;

  // loop over all joints
  for (map<string, double>::const_iterator jnt=joint_positions.begin(); jnt != joint_positions.end(); jnt++) {
    std::map<std::string, SegmentPair>::const_iterator seg = segments_.find(jnt->first);
    if (seg != segments_.end()) {
      geometry_msgs::TransformStamped tf_transform = tf2::kdlToTransform(seg->second.segment.pose(jnt->second));
      tf_transform.header.stamp = time;
      tf_transform.header.frame_id = stripSlash(seg->second.root);
      tf_transform.child_frame_id = stripSlash(seg->second.tip);
      tf_transforms.push_back(tf_transform);
    }
    else {
      ROS_WARN_THROTTLE(10, "Joint state with name: \"%s\" was received but not found in URDF", jnt->first.c_str());
    }
  }
  tf_broadcaster_.sendTransform(tf_transforms);
}

// publish fixed transforms
void RobotStatePublisher::publishFixedTransforms(bool use_tf_static)
{
  boost::unique_lock<boost::shared_mutex> lock(m_swapMutex, boost::try_to_lock);
  if (!lock.owns_lock())
  {
    ROS_DEBUG("Publishing transforms for fixed joints -- could not get lock");
    return;
  }
  ROS_DEBUG("Publishing transforms for fixed joints");
  std::vector<geometry_msgs::TransformStamped> tf_transforms;
  geometry_msgs::TransformStamped tf_transform;

  // loop over all fixed segments
  for (map<string, SegmentPair>::const_iterator seg=segments_fixed_.begin(); seg != segments_fixed_.end(); seg++) {
    geometry_msgs::TransformStamped tf_transform = tf2::kdlToTransform(seg->second.segment.pose(0));
    tf_transform.header.stamp = ros::Time::now();
    if (!use_tf_static) {
      tf_transform.header.stamp += ros::Duration(0.5);
    }
    tf_transform.header.frame_id = stripSlash(seg->second.root);
    tf_transform.child_frame_id = stripSlash(seg->second.tip);
    tf_transforms.push_back(tf_transform);
  }
  if (use_tf_static) {
    static_tf_broadcaster_.sendTransform(tf_transforms);
  }
  else {
    tf_broadcaster_.sendTransform(tf_transforms);
  }
}

}
