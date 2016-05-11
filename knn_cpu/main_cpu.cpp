#include <iostream>
#include <vector>
#include <float.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <CL/opencl.h>
#include <algorithm>
#include <sys/time.h>

#define CL_USE_DEPRECATED_OPENCL_1_1_APIS // to sort compatibility issues b/w various OCL libraries
#define REC_LENGTH 49 // size of a record in the filelist
#define LOCAL_SIZE 256 // work group size set here!

typedef struct record
{
  char recString[REC_LENGTH];
  float distance;
} Record;

void findLowest(std::vector<Record> &records,float *distances,int numRecords,int topN) {
  int i,j;
  float val;
  int minLoc;
  Record *tempRec;
  float tempDist;
  
  for(i=0;i<topN;i++) {
    minLoc = i;
    for(j=i;j<numRecords;j++) {
      val = distances[j];
      if (val < distances[minLoc]) minLoc = j;
    }

    // swap locations and distances
    tempRec = &records[i];
    records[i] = records[minLoc];
    records[minLoc] = *tempRec;


    tempDist = distances[i];
    distances[i] = distances[minLoc];
    distances[minLoc] = tempDist;

    // add distance to the min we just found
    records[i].distance = distances[i];
  }
}
  
int load_file_to_memory(const char *filename, char **result) { 
  size_t size = 0;
  FILE *f = fopen(filename, "rb");
  if (f == NULL) 
  { 
    *result = NULL;
    return -1; // -1 means file opening fail 
  } 
  fseek(f, 0, SEEK_END);
  size = ftell(f);
  fseek(f, 0, SEEK_SET);
  *result = (char *)malloc(size+1);
  if (size != fread(*result, sizeof(char), size, f)) 
  { 
    free(*result);
    return -2; // -2 means file reading fail 
  } 
  fclose(f);
  (*result)[size] = 0;
  return size;
}

int main(int argc, char *argv[]) {
  std::vector<Record> records;
  std::vector<cl_float2> locations;
  int i;
  char filename[100];
  int resultsCount=5,quiet=0,timing=0;
  float lat=30.0,lng=90.0;
  
  cl_context context;
  cl_context_properties properties[3];
  cl_kernel kernel;
  cl_command_queue command_queue;
  cl_program program;
  cl_int err;
  cl_uint num_of_platforms=0;
  cl_platform_id platform_id;
  cl_device_id device_id;
  cl_uint num_of_devices=0;
       
  FILE *fp;
  int numRecords=0;
  fp = fopen("../../../../../data/filelist.txt", "r");
  if (!fp) {
    perror("error opening the data file\n");
    exit(1);
  }
  
  // read each record
  while(!feof(fp)) {
    Record record;
    cl_float2 latLong;
    fgets(record.recString,49,fp);
    fgetc(fp); // newline
    if (feof(fp)) break;
  
    // parse for lat and long
    char str[49];
    strncpy(str,record.recString,sizeof(str));
    int year, month, date, hour, num, speed, press;
    float lat, lon;
    char name[30];
    sscanf(str, "%d %d %d %d %d %s %f %f %d %d", &year, 
      &month, &date, &hour, &num, name, &lat,   &lon, &speed, &press);    
    latLong.x = lat;
    latLong.y = lon;        
                      
    locations.push_back(latLong);
    records.push_back(record);
    numRecords++;
  }
  fclose(fp);

  if (!quiet) {
    printf("Number of records: %d\n",numRecords);
    printf("latitude: %f\n",lat);
    printf("longitude: %f\n",lng);
    printf("Finding the %d closest neighbors.\n",resultsCount);
  }

  if (resultsCount > numRecords) resultsCount = numRecords;

  // retreive a list of platforms avaible
  if (clGetPlatformIDs(1, &platform_id, &num_of_platforms)!= CL_SUCCESS) {
    printf("Unable to get platform_id\n");
    return 1;
  }

  // connect to a compute device	
  if (clGetDeviceIDs(platform_id, CL_DEVICE_TYPE_ACCELERATOR, 1, 
    &device_id, &num_of_devices) != CL_SUCCESS) {
    printf("Unable to get device_id\n");
    return 1;
  }

  // context properties list - must be terminated with 0
  properties[0]= CL_CONTEXT_PLATFORM;
  properties[1]= (cl_context_properties) platform_id;
  properties[2]= 0;

  // create a context with the FPGA device
  context = clCreateContext(properties,1,&device_id,NULL,NULL,&err);

  // create command queue using the context and device
  command_queue = clCreateCommandQueue(context, device_id, CL_QUEUE_PROFILING_ENABLE, &err);
  
  int status;
  unsigned char *kernelbinary;
  char *xclbin=argv[1];
  printf("loading %s\n", xclbin);
  int n_i = load_file_to_memory(xclbin, (char **) &kernelbinary);
  if (n_i < 0) {
    printf("failed to load kernel from xclbin: %s\n", xclbin);
    printf("Test failed\n");
    return EXIT_FAILURE;
  }
  size_t n = n_i;
  
  // Create the compute program from offline
  program = clCreateProgramWithBinary(context, 1, &device_id, &n,
    (const unsigned char **) &kernelbinary, &status, &err);
  // compile the program
  if (clBuildProgram(program, 0, NULL, NULL, NULL, NULL) != CL_SUCCESS) {
  printf("Error building program\n");
  return 1;
  }
  
  // specify which kernel from the program to execute
  kernel = clCreateKernel(program, "NearestNeighbor", &err);
 
  // create buffers for the input and ouput
  cl_mem d_locations;
  cl_mem d_distances;
  cl_event writeEvent,kernelEvent,readEvent;
  cl_int error=0;

  d_locations = clCreateBuffer(context, CL_MEM_READ_ONLY,
    sizeof(cl_float2) * numRecords, NULL, NULL);
  d_distances = clCreateBuffer(context, CL_MEM_READ_WRITE,
    sizeof(float) * numRecords, NULL, NULL);
    
  // load data into the input buffer
  clEnqueueWriteBuffer(command_queue, d_locations, CL_TRUE, 0, 
    sizeof(cl_float2) * numRecords,   &locations[0], 0, NULL, &writeEvent);

  // set the argument list for the kernel command
  clSetKernelArg(kernel, 0, sizeof(cl_mem), (void *)&d_locations);
  clSetKernelArg(kernel, 1, sizeof(cl_mem), (void *)&d_distances);
  clSetKernelArg(kernel, 2, sizeof(float), (void *)&lat);
  clSetKernelArg(kernel, 3, sizeof(float), (void *)&lng);
     
  // enqueue kernel for execution
  size_t globalWorkSize[1],localWorkSize[1];
  globalWorkSize[0] = numRecords;
  localWorkSize[0] = LOCAL_SIZE;
  if (numRecords % 64) globalWorkSize[0] += 64 - (numRecords % 64);
          
  clEnqueueNDRangeKernel(command_queue, kernel, 1, NULL, 
    (size_t*)&globalWorkSize, (size_t*)&localWorkSize, 0, NULL, &kernelEvent);
  //clFinish(command_queue);

  // copy the results from out of the output buffer
  float *distances = (float *)malloc(sizeof(float) * numRecords);
  clEnqueueReadBuffer(command_queue, d_distances, CL_TRUE, 0, 
    sizeof(float) * numRecords, distances, 0, NULL, &readEvent);

  if (timing) {
    clFinish(command_queue);
    cl_ulong eventStart,eventEnd,totalTime=0;
    printf("# Records\tWrite(s) [size]\t\tKernel(s)\tRead(s)  [size]\t\tTotal(s)\n");
    printf("%d        \t",numRecords);
    
    // Write Buffer
    error = clGetEventProfilingInfo(writeEvent,CL_PROFILING_COMMAND_START,
      sizeof(cl_ulong),&eventStart,NULL);
    error = clGetEventProfilingInfo(writeEvent,CL_PROFILING_COMMAND_END,
      sizeof(cl_ulong),&eventEnd,NULL);
    printf("%f [%.2fMB]\t",(float)((eventEnd-eventStart)/1e9),
      (float)((sizeof(cl_float2) * numRecords)/1e6));
    totalTime += eventEnd-eventStart;

    // Kernel
    error = clGetEventProfilingInfo(kernelEvent,CL_PROFILING_COMMAND_START,
      sizeof(cl_ulong),&eventStart,NULL);
    error = clGetEventProfilingInfo(kernelEvent,CL_PROFILING_COMMAND_END,
      sizeof(cl_ulong),&eventEnd,NULL);
    printf("%f\t",(float)((eventEnd-eventStart)/1e9));
    totalTime += eventEnd-eventStart;

    // Read Buffer
    error = clGetEventProfilingInfo(readEvent,CL_PROFILING_COMMAND_START,
      sizeof(cl_ulong),&eventStart,NULL);
    error = clGetEventProfilingInfo(readEvent,CL_PROFILING_COMMAND_END,
      sizeof(cl_ulong),&eventEnd,NULL);
    printf("%f [%.2fMB]\t",(float)((eventEnd-eventStart)/1e9),
      (float)((sizeof(float) * numRecords)/1e6));
    totalTime += eventEnd-eventStart;
        
    printf("%f\n\n",(float)(totalTime/1e9));
  }

  // return finalized data and release buffers
  clReleaseMemObject(d_locations);
  clReleaseMemObject(d_distances);
  clReleaseProgram(program);
  clReleaseKernel(kernel);
  clReleaseCommandQueue(command_queue);
  clReleaseContext(context);

  // find the "resultsCount" number of nearest neghbors using distances calculated
  struct timeval t0,t1;
  gettimeofday(&t0, NULL);

  findLowest(records,distances,numRecords,resultsCount);

  gettimeofday(&t1, NULL);
  double time_seconds=(t1.tv_sec-t0.tv_sec)+0.000001*(t1.tv_usec-t0.tv_usec);
  printf("Sort time on CPU in seconds = %1f\n",time_seconds);

  // print out results
  if (!quiet)
    for(i=0;i<resultsCount;i++) {
      printf("%s --> Distance=%f\n",records[i].recString,records[i].distance);
    }
  return 0;
}


