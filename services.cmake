###
# If not stated otherwise in this file or this component's LICENSE
# file the following copyright and licenses apply:
#
# Copyright 2026 RDK Management
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
###

option(PLUGIN_EYILGATEWAY "Build the EyilGateway Thunder plugin" ON)
option(BUILD_EYILCLIENT "Build the independent EyilClient shared library" ON)
option(RDK_SERVICES_L1_TEST "Enable the L1 test suite" ON)
option(RDK_SERVICE_L2_TEST "Enable the L2 test suite" ON)
option(ENABLE_COVERAGE "Instrument handwritten code for GCC coverage" OFF)

set(PLUGIN_EYILGATEWAY_MODE "Off" CACHE STRING "Thunder implementation mode: Off or Local")
set_property(CACHE PLUGIN_EYILGATEWAY_MODE PROPERTY STRINGS Off Local)

set(PLUGIN_EYILGATEWAY_AUTOSTART "false" CACHE STRING "Autostart org.rdk.EyilGateway")
set_property(CACHE PLUGIN_EYILGATEWAY_AUTOSTART PROPERTY STRINGS true false)

set(PLUGIN_EYILGATEWAY_STARTUPORDER "" CACHE STRING "EyilGateway startup order")

