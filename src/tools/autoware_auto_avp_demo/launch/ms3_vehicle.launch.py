# Copyright 2020, The Autoware Foundation
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""Launch Modules for Milestone 3 of the AVP 2020 Demo."""

from launch import LaunchContext
from launch import LaunchDescription
from launch.actions import DeclareLaunchArgument
from launch.actions import IncludeLaunchDescription
from launch.launch_description_sources import PythonLaunchDescriptionSource
from launch.substitutions import LaunchConfiguration
from launch_ros.actions import Node
from launch_ros.substitutions import FindPackage
from pathlib import Path

import os


context = LaunchContext()


def get_package_share_directory(package_name):
    """Return the absolute path to the share directory of the given package."""
    return os.path.join(Path(FindPackage(package_name).perform(context)), 'share', package_name)


def generate_launch_description():
    """
    Launch all nodes defined in the architecture for Milestone 3 of the AVP 2020 Demo.

    More details about what is included can
    be found at https://gitlab.com/autowarefoundation/autoware.auto/AutowareAuto/-/milestones/25.
    """
    avp_demo_pkg_prefix = get_package_share_directory('autoware_auto_avp_demo')
    map_publisher_param_file = os.path.join(
        avp_demo_pkg_prefix, 'param/map_publisher_vehicle.param.yaml')
    ndt_localizer_param_file = os.path.join(
        avp_demo_pkg_prefix, 'param/ndt_localizer_vehicle.param.yaml')
    mpc_param_file = os.path.join(
        avp_demo_pkg_prefix, 'param/mpc_vehicle.param.yaml')
    vlp16_front_param_file = os.path.join(
        avp_demo_pkg_prefix, 'param/vlp16_front_vehicle.param.yaml')
    vlp16_rear_param_file = os.path.join(
        avp_demo_pkg_prefix, 'param/vlp16_rear_vehicle.param.yaml')

    urdf_pkg_prefix = get_package_share_directory('lexus_rx_450h_description')
    urdf_path = os.path.join(urdf_pkg_prefix, 'urdf/lexus_rx_450h.urdf')

    # Arguments

    vlp16_front_param = DeclareLaunchArgument(
        'vlp16_front_param_file',
        default_value=vlp16_front_param_file,
        description='Path to config file for front Velodyne'
    )
    vlp16_rear_param = DeclareLaunchArgument(
        'vlp16_rear_param_file',
        default_value=vlp16_rear_param_file,
        description='Path to config file for rear Velodyne'
    )
    map_publisher_param = DeclareLaunchArgument(
        'map_publisher_param_file',
        default_value=map_publisher_param_file,
        description='Path to config file for Map Publisher'
    )
    ndt_localizer_param = DeclareLaunchArgument(
        'ndt_localizer_param_file',
        default_value=ndt_localizer_param_file,
        description='Path to config file for ndt localizer'
    )
    mpc_param = DeclareLaunchArgument(
        'mpc_param_file',
        default_value=mpc_param_file,
        description='Path to config file for MPC'
    )

    # Nodes

    vlp16_front = Node(
        package='velodyne_node',
        node_executable='velodyne_cloud_node_exe',
        node_namespace='lidar_front',
        parameters=[LaunchConfiguration('vlp16_front_param_file')],
        arguments=["--model", "vlp16"]
    )
    vlp16_rear = Node(
        package='velodyne_node',
        node_executable='velodyne_cloud_node_exe',
        node_namespace='lidar_rear',
        parameters=[LaunchConfiguration('vlp16_rear_param_file')],
        arguments=["--model", "vlp16"]
    )
    map_publisher = Node(
        package='ndt_nodes',
        node_executable='ndt_map_publisher_exe',
        node_namespace='localization',
        parameters=[LaunchConfiguration('map_publisher_param_file')]
    )
    urdf_publisher = Node(
        package='robot_state_publisher',
        node_executable='robot_state_publisher',
        node_name='robot_state_publisher',
        arguments=[str(urdf_path)]
    )
    ndt_localizer = Node(
        package='ndt_nodes',
        node_executable='p2d_ndt_localizer_exe',
        node_namespace='localization',
        node_name='p2d_ndt_localizer_node',
        parameters=[LaunchConfiguration('ndt_localizer_param_file')],
        remappings=[
            ("points_in", "/lidars/points_fused_downsampled")
        ]
    )
    mpc = Node(
        package='mpc_controller_node',
        node_executable='mpc_controller_node_exe',
        node_name='mpc_controller',
        node_namespace='control',
        parameters=[LaunchConfiguration('mpc_param_file')]
    )

    core_launch = IncludeLaunchDescription(
        PythonLaunchDescriptionSource([avp_demo_pkg_prefix, '/launch/ms3_core.launch.py'])
    )

    return LaunchDescription([
        vlp16_front_param,
        vlp16_rear_param,
        map_publisher_param,
        ndt_localizer_param,
        mpc_param,
        vlp16_front,
        vlp16_rear,
        urdf_publisher,
        map_publisher,
        ndt_localizer,
        mpc,
        core_launch
    ])