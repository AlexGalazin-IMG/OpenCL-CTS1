//
// Copyright (c) 2023 The Khronos Group Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <vector>

#include "testBase.h"

// clang-format off

static const char *async_strided_global_to_local_kernel =
"%s\n" // optional pragma string
"__kernel void test_fn( const __global %s *src, __global %s *dst, __local %s *localBuffer, int copiesPerWorkgroup, int copiesPerWorkItem, int stride )\n"
"{\n"
" int i;\n"
// Zero the local storage first
" for(i=0; i<copiesPerWorkItem; i++)\n"
"   localBuffer[ get_local_id( 0 )*copiesPerWorkItem+i ] = (%s)(%s)0;\n"
// Do this to verify all kernels are done zeroing the local buffer before we try the copy
" barrier( CLK_LOCAL_MEM_FENCE );\n"
" event_t event;\n"
" event = async_work_group_strided_copy( (__local %s*)localBuffer, (__global const %s*)(src+copiesPerWorkgroup*stride*get_group_id(0)), (size_t)copiesPerWorkgroup, (size_t)stride, 0 );\n"
// Wait for the copy to complete, then verify by manually copying to the dest
" wait_group_events( 1, &event );\n"
" for(i=0; i<copiesPerWorkItem; i++)\n"
"   dst[ get_global_id( 0 )*copiesPerWorkItem*stride+i*stride ] = localBuffer[ get_local_id( 0 )*copiesPerWorkItem+i ];\n"
"}\n" ;

static const char *async_strided_local_to_global_kernel =
"%s\n" // optional pragma string
"__kernel void test_fn( const __global %s *src, __global %s *dst, __local %s *localBuffer, int copiesPerWorkgroup, int copiesPerWorkItem, int stride )\n"
"{\n"
" int i;\n"
// Zero the local storage first
" for(i=0; i<copiesPerWorkItem; i++)\n"
"   localBuffer[ get_local_id( 0 )*copiesPerWorkItem+i ] = (%s)(%s)0;\n"
// Do this to verify all kernels are done zeroing the local buffer before we try the copy
" barrier( CLK_LOCAL_MEM_FENCE );\n"
" for(i=0; i<copiesPerWorkItem; i++)\n"
"   localBuffer[ get_local_id( 0 )*copiesPerWorkItem+i ] = src[ get_global_id( 0 )*copiesPerWorkItem*stride+i*stride ];\n"
// Do this to verify all kernels are done copying to the local buffer before we try the copy
" barrier( CLK_LOCAL_MEM_FENCE );\n"
" event_t event;\n"
" event = async_work_group_strided_copy((__global %s*)(dst+copiesPerWorkgroup*stride*get_group_id(0)), (__local const %s*)localBuffer, (size_t)copiesPerWorkgroup, (size_t)stride, 0 );\n"
" wait_group_events( 1, &event );\n"
"}\n" ;

// clang-format on

static int test_strided_copy(cl_device_id deviceID, cl_context context,
                             cl_command_queue queue, const char *kernelCode,
                             ExplicitType vecType, int vecSize, int stride)
{
    int error;
    clProgramWrapper program;
    clKernelWrapper kernel;
    clMemWrapper streams[ 2 ];
    size_t threads[ 1 ], localThreads[ 1 ];
    MTdataHolder d(gRandomSeed);
    char vecNameString[64]; vecNameString[0] = 0;

    if (vecSize == 1)
        sprintf(vecNameString, "%s", get_explicit_type_name(vecType));
    else
        sprintf(vecNameString, "%s%d", get_explicit_type_name(vecType), vecSize);


    log_info("Testing %s\n", vecNameString);

    cl_long max_local_mem_size;
    error = clGetDeviceInfo(deviceID, CL_DEVICE_LOCAL_MEM_SIZE, sizeof(max_local_mem_size), &max_local_mem_size, NULL);
    test_error( error, "clGetDeviceInfo for CL_DEVICE_LOCAL_MEM_SIZE failed.");

    unsigned int num_of_compute_devices;
    error = clGetDeviceInfo(deviceID, CL_DEVICE_MAX_COMPUTE_UNITS, sizeof(num_of_compute_devices), &num_of_compute_devices, NULL);
    test_error( error, "clGetDeviceInfo for CL_DEVICE_MAX_COMPUTE_UNITS failed.");

    char programSource[4096]; programSource[0]=0;
    char *programPtr;

    std::string extStr = "";
    if (vecType == kDouble)
        extStr = "#pragma OPENCL EXTENSION cl_khr_fp64 : enable";
    else if (vecType == kHalf)
        extStr = "#pragma OPENCL EXTENSION cl_khr_fp16 : enable";

    sprintf(programSource, kernelCode, extStr.c_str(), vecNameString,
            vecNameString, vecNameString, vecNameString,
            get_explicit_type_name(vecType), vecNameString, vecNameString);
    //log_info("program: %s\n", programSource);
    programPtr = programSource;

    error = create_single_kernel_helper( context, &program, &kernel, 1, (const char **)&programPtr, "test_fn" );
    test_error( error, "Unable to create testing kernel" );

    size_t max_workgroup_size;
    error = clGetKernelWorkGroupInfo(kernel, deviceID, CL_KERNEL_WORK_GROUP_SIZE, sizeof(max_workgroup_size), &max_workgroup_size, NULL);
    test_error (error, "clGetKernelWorkGroupInfo failed for CL_KERNEL_WORK_GROUP_SIZE.");

    size_t max_local_workgroup_size[3];
    error = clGetDeviceInfo(deviceID, CL_DEVICE_MAX_WORK_ITEM_SIZES, sizeof(max_local_workgroup_size), max_local_workgroup_size, NULL);
    test_error (error, "clGetDeviceInfo failed for CL_DEVICE_MAX_WORK_ITEM_SIZES");

  // Pick the minimum of the device and the kernel
    if (max_workgroup_size > max_local_workgroup_size[0])
        max_workgroup_size = max_local_workgroup_size[0];

    size_t elementSize = get_explicit_type_size(vecType)* ((vecSize == 3) ? 4 : vecSize);

    cl_ulong max_global_mem_size;
    error = clGetDeviceInfo(deviceID, CL_DEVICE_GLOBAL_MEM_SIZE, sizeof(max_global_mem_size), &max_global_mem_size, NULL);
    test_error (error, "clGetDeviceInfo failed for CL_DEVICE_GLOBAL_MEM_SIZE");

    if (max_global_mem_size > (cl_ulong)SIZE_MAX) {
      max_global_mem_size = (cl_ulong)SIZE_MAX;
    }

    cl_bool unified_mem;
    error = clGetDeviceInfo(deviceID, CL_DEVICE_HOST_UNIFIED_MEMORY, sizeof(unified_mem), &unified_mem, NULL);
    test_error (error, "clGetDeviceInfo failed for CL_DEVICE_HOST_UNIFIED_MEMORY");

    int number_of_global_mem_buffers = (unified_mem) ? 4 : 2;

    size_t numberOfCopiesPerWorkitem = 3;
    size_t localStorageSpacePerWorkitem = numberOfCopiesPerWorkitem*elementSize;
    size_t maxLocalWorkgroupSize = (((int)max_local_mem_size/2)/localStorageSpacePerWorkitem);

    size_t localWorkgroupSize = maxLocalWorkgroupSize;
    if (maxLocalWorkgroupSize > max_workgroup_size)
        localWorkgroupSize = max_workgroup_size;

    size_t localBufferSize = localWorkgroupSize*elementSize*numberOfCopiesPerWorkitem;
    size_t numberOfLocalWorkgroups = 579;//1111;

    // Reduce the numberOfLocalWorkgroups so that no more than 1/2 of CL_DEVICE_GLOBAL_MEM_SIZE is consumed
    // by the allocated buffer. This is done to avoid resource  errors resulting from address space fragmentation.
    size_t numberOfLocalWorkgroupsLimit = max_global_mem_size / (2 * number_of_global_mem_buffers * localBufferSize * stride);
    if (numberOfLocalWorkgroups > numberOfLocalWorkgroupsLimit) numberOfLocalWorkgroups = numberOfLocalWorkgroupsLimit;

    size_t globalBufferSize = numberOfLocalWorkgroups*localBufferSize*stride;
    size_t globalWorkgroupSize = numberOfLocalWorkgroups*localWorkgroupSize;

    std::vector<unsigned char> inBuffer(globalBufferSize);
    std::vector<unsigned char> outBuffer(globalBufferSize);
    memset(outBuffer.data(), 0, globalBufferSize);

    cl_int copiesPerWorkItemInt, copiesPerWorkgroup;
    copiesPerWorkItemInt = (int)numberOfCopiesPerWorkitem;
    copiesPerWorkgroup = (int)(numberOfCopiesPerWorkitem*localWorkgroupSize);

    log_info("Global: %d, local %d, local buffer %db, global buffer %db, copy stride %d, each work group will copy %d elements and each work item item will copy %d elements.\n",
                (int) globalWorkgroupSize, (int)localWorkgroupSize, (int)localBufferSize, (int)globalBufferSize, (int)stride, copiesPerWorkgroup, copiesPerWorkItemInt);

    threads[0] = globalWorkgroupSize;
    localThreads[0] = localWorkgroupSize;

    generate_random_data(vecType,
                         globalBufferSize / get_explicit_type_size(vecType), d,
                         inBuffer.data());

    streams[0] = clCreateBuffer(context, CL_MEM_COPY_HOST_PTR, globalBufferSize,
                                inBuffer.data(), &error);
    test_error( error, "Unable to create input buffer" );
    streams[1] = clCreateBuffer(context, CL_MEM_COPY_HOST_PTR, globalBufferSize,
                                outBuffer.data(), &error);
    test_error( error, "Unable to create output buffer" );

    error = clSetKernelArg( kernel, 0, sizeof( streams[ 0 ] ), &streams[ 0 ] );
    test_error( error, "Unable to set kernel argument" );
    error = clSetKernelArg( kernel, 1, sizeof( streams[ 1 ] ), &streams[ 1 ] );
    test_error( error, "Unable to set kernel argument" );
    error = clSetKernelArg( kernel, 2, localBufferSize, NULL );
    test_error( error, "Unable to set kernel argument" );
    error = clSetKernelArg( kernel, 3, sizeof(copiesPerWorkgroup), &copiesPerWorkgroup );
    test_error( error, "Unable to set kernel argument" );
    error = clSetKernelArg( kernel, 4, sizeof(copiesPerWorkItemInt), &copiesPerWorkItemInt );
    test_error( error, "Unable to set kernel argument" );
    error = clSetKernelArg( kernel, 5, sizeof(stride), &stride );
    test_error( error, "Unable to set kernel argument" );

    // Enqueue
    error = clEnqueueNDRangeKernel( queue, kernel, 1, NULL, threads, localThreads, 0, NULL, NULL );
    test_error( error, "Unable to queue kernel" );

    // Read
    error = clEnqueueReadBuffer(queue, streams[1], CL_TRUE, 0, globalBufferSize,
                                outBuffer.data(), 0, NULL, NULL);
    test_error( error, "Unable to read results" );

    // Verify
    size_t typeSize = get_explicit_type_size(vecType)* vecSize;
    for (int i=0; i<(int)globalBufferSize; i+=(int)elementSize*(int)stride)
    {
        if (memcmp(&inBuffer.at(i), &outBuffer.at(i), typeSize) != 0)
        {
            unsigned char *inchar =
                static_cast<unsigned char *>(&inBuffer.at(i));
            unsigned char *outchar =
                static_cast<unsigned char *>(&outBuffer.at(i));
            char values[4096];
            values[0] = 0;

            log_error( "ERROR: Results of copy did not validate!\n" );
            sprintf(values + strlen( values), "%d -> [", i);
            for (int j=0; j<(int)elementSize; j++)
                sprintf(values + strlen( values), "%2x ", inchar[j]);
            sprintf(values + strlen(values), "] != [");
            for (int j=0; j<(int)elementSize; j++)
                sprintf(values + strlen( values), "%2x ", outchar[j]);
            sprintf(values + strlen(values), "]");
            log_error("%s\n", values);
            return -1;
        }
    }

    return 0;
}

static int test_strided_copy_all_types(cl_device_id deviceID,
                                       cl_context context,
                                       cl_command_queue queue,
                                       const char *kernelCode)
{
    const std::vector<ExplicitType> vecType = { kChar,  kUChar, kShort, kUShort,
                                                kInt,   kUInt,  kLong,  kULong,
                                                kFloat, kHalf,  kDouble };
    const unsigned int vecSizes[] = { 1, 2, 3, 4, 8, 16, 0 };
    const unsigned int strideSizes[] = { 1, 3, 4, 5, 0 };
    unsigned int size, typeIndex, stride;

    int errors = 0;

    bool fp16Support = is_extension_available(deviceID, "cl_khr_fp16");
    bool fp64Support = is_extension_available(deviceID, "cl_khr_fp64");

    for (typeIndex = 0; typeIndex < vecType.size(); typeIndex++)
    {
        if (( vecType[ typeIndex ] == kLong || vecType[ typeIndex ] == kULong ) && !gHasLong )
            continue;
        else if (vecType[typeIndex] == kDouble && !fp64Support)
            continue;
        else if (vecType[typeIndex] == kHalf && !fp16Support)
            continue;

        for( size = 0; vecSizes[ size ] != 0; size++ )
        {
            for( stride = 0; strideSizes[ stride ] != 0; stride++)
            {
                if (test_strided_copy( deviceID, context, queue, kernelCode, vecType[typeIndex], vecSizes[size], strideSizes[stride] ))
                {
                    errors++;
                }
            }
        }
    }
    if (errors)
        return -1;
    return 0;
}

REGISTER_TEST(async_strided_copy_global_to_local)
{
    return test_strided_copy_all_types(device, context, queue,
                                       async_strided_global_to_local_kernel);
}

REGISTER_TEST(async_strided_copy_local_to_global)
{
    return test_strided_copy_all_types(device, context, queue,
                                       async_strided_local_to_global_kernel);
}
