// Copyright 2018 the Autoware Foundation
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// Co-developed by Tier IV, Inc. and Apex.AI, Inc.

#include <string>
#include <chrono>
#include <memory>
#include <vector>

#include "common/types.hpp"
#include "lidar_utils/point_cloud_utils.hpp"
#include "point_cloud_msg_wrapper/point_cloud_msg_wrapper.hpp"
#include "sensor_msgs/point_cloud2_iterator.hpp"
#include "velodyne_nodes/velodyne_cloud_node.hpp"

#include "rclcpp_components/register_node_macro.hpp"

using autoware::common::types::bool8_t;
using autoware::common::types::float32_t;

namespace autoware
{
namespace drivers
{
namespace velodyne_nodes
{

template<typename CloudModifierT>
void CloudModifierWrapper<CloudModifierT>::clear()
{
  return modifier_.clear();
}

template<typename CloudModifierT>
void CloudModifierWrapper<CloudModifierT>::reserve(const std::size_t cloud_size)
{
  return modifier_.reserve(cloud_size);
}

template<typename CloudModifierT>
void CloudModifierWrapper<CloudModifierT>::resize(const uint32_t cloud_size)
{
  return modifier_.resize(cloud_size);
}

template<typename CloudModifierT>
std::size_t CloudModifierWrapper<CloudModifierT>::size() const
{
  return modifier_.size();
}

template<>
void CloudModifierWrapper<autoware::common::lidar_utils::CloudModifierRing>::push_back(
  const autoware::common::types::PointXYZIF & pt)
{
  return modifier_.push_back(pt);
}

template<>
void CloudModifierWrapper<autoware::common::lidar_utils::CloudModifier>::push_back(
  const autoware::common::types::PointXYZIF & pt)
{
  return modifier_.push_back(autoware::common::types::PointXYZI{pt.x, pt.y, pt.z, pt.intensity});
}

template<typename T>
VelodyneCloudNode<T>::VelodyneCloudNode(
  const std::string & node_name,
  const rclcpp::NodeOptions & options)
: rclcpp::Node(node_name, options),
  m_io_cxt(),
  m_udp_driver(m_io_cxt),
  m_translator(Config{static_cast<float32_t>(this->declare_parameter("rpm").template get<int>())}),
  m_ip(this->declare_parameter("ip").template get<std::string>().c_str()),
  m_port(static_cast<uint16_t>(this->declare_parameter("port").template get<uint16_t>())),
  m_pc2_pub_ptr(create_publisher<sensor_msgs::msg::PointCloud2>(
      declare_parameter("topic").template
      get<std::string>(), rclcpp::QoS{10})),
  m_remainder_start_idx(0U),
  m_point_cloud_idx(0),
  m_frame_id(this->declare_parameter("frame_id").template get<std::string>().c_str()),
  m_cloud_size(static_cast<std::uint32_t>(
      this->declare_parameter("cloud_size").template get<std::uint32_t>())),
  m_ring_information(this->declare_parameter("ring_information").template get<bool8_t>())
{
  m_point_block.reserve(VelodyneTranslatorT::POINT_BLOCK_CAPACITY);
  // If your preallocated cloud size is too small, the node really won't operate well at all
  if (static_cast<uint32_t>(m_point_block.capacity()) >= m_cloud_size) {
    throw std::runtime_error("VelodyneCloudNode: cloud_size must be > PointBlock::CAPACITY");
  }

  init_udp_driver();
  init_output(m_pc2_msg);
}

template<typename T>
void VelodyneCloudNode<T>::init_udp_driver()
{
  m_udp_driver.init_receiver(m_ip, m_port);
  m_udp_driver.receiver()->open();
  m_udp_driver.receiver()->bind();
  m_udp_driver.receiver()->asyncReceive(
    std::bind(&VelodyneCloudNode<T>::receiver_callback, this, std::placeholders::_1));
}

template<typename T>
void VelodyneCloudNode<T>::receiver_callback(const std::vector<uint8_t> & buffer)
{
  Packet pkt{};
  std::memcpy(&pkt, &buffer[0], buffer.size());
  try {
    // message received, convert and publish
    if (this->convert(pkt, m_pc2_msg)) {
      m_pc2_pub_ptr->publish(m_pc2_msg);
      while (this->get_output_remainder(m_pc2_msg)) {
        m_pc2_pub_ptr->publish(m_pc2_msg);
      }
    }
  } catch (const std::exception & e) {
    RCLCPP_WARN(this->get_logger(), e.what());
    // And then just continue running
  } catch (...) {
    // Something really weird happened and I can't handle it here
    RCLCPP_WARN(this->get_logger(), "Unknown exception occured in VelodyneCloudNode");
    throw;
  }
}
////////////////////////////////////////////////////////////////////////////////
template<typename T>
void VelodyneCloudNode<T>::init_output(sensor_msgs::msg::PointCloud2 & output)
{
  using autoware::common::lidar_utils::CloudModifierRing;
  using autoware::common::lidar_utils::CloudModifier;

  std::shared_ptr<CloudModifierWrapperBase> modifier = nullptr;
  if (m_ring_information) {
    modifier = std::make_shared<CloudModifierWrapper<CloudModifierRing>>(output, m_frame_id);
  } else {
    modifier = std::make_shared<CloudModifierWrapper<CloudModifier>>(output, m_frame_id);
  }
  modifier->reserve(m_cloud_size);
}

////////////////////////////////////////////////////////////////////////////////
template<typename T>
bool8_t VelodyneCloudNode<T>::convert(
  const Packet & pkt,
  sensor_msgs::msg::PointCloud2 & output)
{
  using autoware::common::lidar_utils::CloudModifierRing;
  using autoware::common::lidar_utils::CloudModifier;

  // This handles the case when the below loop exited due to containing extra points
  using autoware::common::types::PointXYZIF;

  std::shared_ptr<CloudModifierWrapperBase> modifier = nullptr;
  if (m_ring_information) {
    modifier = std::make_shared<CloudModifierWrapper<CloudModifierRing>>(output);
  } else {
    modifier = std::make_shared<CloudModifierWrapper<CloudModifier>>(output);
  }

  if (m_published_cloud) {
    // reset the pointcloud
    modifier->clear();
    modifier->reserve(m_cloud_size);
    m_point_cloud_idx = 0;

    // deserialize remainder into pointcloud
    m_published_cloud = false;
    for (uint32_t idx = m_remainder_start_idx; idx < m_point_block.size(); ++idx) {
      const autoware::common::types::PointXYZIF & pt = m_point_block[idx];
      modifier->push_back(pt);
      m_point_cloud_idx++;
    }
  }
  m_translator.convert(pkt, m_point_block);
  for (uint32_t idx = 0U; idx < m_point_block.size(); ++idx) {
    const autoware::common::types::PointXYZIF & pt = m_point_block[idx];
    if (static_cast<uint16_t>(autoware::common::types::PointXYZIF::END_OF_SCAN_ID) != pt.id) {
      modifier->push_back(pt);
      m_point_cloud_idx++;
      if (modifier->size() >= m_cloud_size) {
        m_published_cloud = true;
        m_remainder_start_idx = idx;
      }
    } else {
      m_published_cloud = true;
      m_remainder_start_idx = idx;
      break;
    }
  }
  if (m_published_cloud) {
    // resize pointcloud down to its actual size
    modifier->resize(m_point_cloud_idx);
    output.header.stamp = this->now();
  }

  return m_published_cloud;
}

////////////////////////////////////////////////////////////////////////////////
template<typename T>
bool8_t VelodyneCloudNode<T>::get_output_remainder(sensor_msgs::msg::PointCloud2 & output)
{
  // The assumption checked in the constructor is that the PointCloud size is bigger than
  // the PointBlocks, which can fully contain a packet. The use case of this method is in case
  // PacketT > OutputT, which is not the case here.
  (void)output;
  return false;
}

VLP16DriverNode::VLP16DriverNode(const rclcpp::NodeOptions & node_options)
: VelodyneCloudNode<velodyne_driver::VLP16Data>("vlp16_driver_node", node_options) {}
VLP32CDriverNode::VLP32CDriverNode(const rclcpp::NodeOptions & node_options)
: VelodyneCloudNode<velodyne_driver::VLP32CData>("vlp32c_driver_node", node_options) {}
VLS128DriverNode::VLS128DriverNode(const rclcpp::NodeOptions & node_options)
: VelodyneCloudNode<velodyne_driver::VLS128Data>("vls128_driver_node", node_options) {}

VelodyneCloudWrapperNode::VelodyneCloudWrapperNode(const rclcpp::NodeOptions & node_options)
: rclcpp::Node("velodyne_cloud_node_wrapper", node_options)
{
  std::string model = declare_parameter("model").get<std::string>();

  if (model == "vlp16") {
    vlp16_driver_node_ptr_ = std::make_shared<VelodyneCloudNode<velodyne_driver::VLP16Data>>(
      "vlp16_driver_node", node_options);
  } else if (model == "vlp32c") {
    vlp32c_driver_node_ptr_ = std::make_shared<VelodyneCloudNode<velodyne_driver::VLP32CData>>(
      "vlp32c_driver_node", node_options);
  } else if (model == "vls128") {
    vls128_driver_node_ptr_ = std::make_shared<VelodyneCloudNode<velodyne_driver::VLS128Data>>(
      "vls128_driver_node", node_options);
  } else {
    throw std::runtime_error("Model " + model + " is not supported.");
  }
}

}  // namespace velodyne_nodes
}  // namespace drivers
}  // namespace autoware

RCLCPP_COMPONENTS_REGISTER_NODE(autoware::drivers::velodyne_nodes::VLP16DriverNode)
RCLCPP_COMPONENTS_REGISTER_NODE(autoware::drivers::velodyne_nodes::VLP32CDriverNode)
RCLCPP_COMPONENTS_REGISTER_NODE(autoware::drivers::velodyne_nodes::VLS128DriverNode)
RCLCPP_COMPONENTS_REGISTER_NODE(autoware::drivers::velodyne_nodes::VelodyneCloudWrapperNode)
