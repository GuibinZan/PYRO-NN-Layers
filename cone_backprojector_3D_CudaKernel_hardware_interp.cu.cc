#if GOOGLE_CUDA
#define EIGEN_USE_GPU
#include "third_party/eigen3/unsupported/Eigen/CXX11/Tensor"
#include "tensorflow/core/framework/op_kernel.h"
#include "helper_headers/helper_grid.h"
#include "helper_headers/helper_math.h"

#define BLOCKSIZE_X           16
#define BLOCKSIZE_Y           4
#define BLOCKSIZE_Z           4

texture<float, cudaTextureType2DLayered> sinogram_as_texture;

#define CUDART_INF_F __int_as_float(0x7f800000)

#define gpuErrchk(ans) { gpuAssert((ans), __FILE__, __LINE__); }
inline void gpuAssert(cudaError_t code, const char *file, int line, bool abort=true)
{
   if (code != cudaSuccess) 
   {
      fprintf(stderr,"GPUassert: %s %s %d\n", cudaGetErrorString(code), file, line);
      if (abort) exit(code);
   }
}

inline __device__ float3 map( float3 coordinates, float* d_projection_matrices, int n )
{
   const float* matrix = &(d_projection_matrices[n*12]);

   return make_float3(
         matrix[0] * coordinates.x + matrix[1] * coordinates.y + matrix[2] * coordinates.z + matrix[3],
         matrix[4] * coordinates.x + matrix[5] * coordinates.y + matrix[6] * coordinates.z + matrix[7],
         matrix[8] * coordinates.x + matrix[9] * coordinates.y + matrix[10] * coordinates.z + matrix[11]
   );
}

__global__ void backproject_3Dcone_beam_kernel_tex_interp( float* vol, float* d_projection_matrices, const int number_of_projections,
                                                const uint3 volume_size, const float3 volume_spacing, const float3 volume_origin, 
                                                const float projection_multiplier)
{
   const int i = blockIdx.x*blockDim.x + threadIdx.x;
   const int j = blockIdx.y*blockDim.y + threadIdx.y;
   const int k = blockIdx.z*blockDim.z + threadIdx.z;
   
   if( i >= volume_size.x  || j >= volume_size.y || k >= volume_size.z )
      return;
   
   const float3 coordinates = index_to_physical(make_float3(i,j,k),volume_origin,volume_spacing); 

   float val = 0.0f;
   
   for( int n = 0; n < number_of_projections; ++n )
   {
      auto ip = map(coordinates , d_projection_matrices, n );

      ip.z = 1.0f / (float)ip.z;
      ip.x *= ip.z;
      ip.y *= ip.z;
      val += tex2DLayered( sinogram_as_texture, ip.x + 0.5, ip.y + 0.5, n ) *  ip.z *  ip.z;
   }

   // linear volume address
   const unsigned int l = volume_size.x * ( k*volume_size.y + j ) + i;
   vol[l] = (val * projection_multiplier);
}

/*************** WARNING ******************./
    * 
    *   Tensorflow is allocating the whole GPU memory for itself and just leave a small slack memory
    *   using cudaMalloc and cudaMalloc3D will allocate memory in this small slack memory !
    *   Therefore, currently only small volumes can be used (they have to fit into the slack memory which TF does not allocae !)
    * 
    *   This is the kernel based on texture interpolation, thus, the allocations are not within the Tensorflow managed memory.
    *   If memory errors occure:
    *    1. start Tensorflow with less gpu memory and allow growth
    *    2. switch to software-based interpolation. 
    * 
    *   TODO: use context->allocate_tmp and context->allocate_persistent instead of cudaMalloc for the projection_matrices array
    *       : https://stackoverflow.com/questions/48580580/tensorflow-new-op-cuda-kernel-memory-managment
    * 
    */
void Cone_Backprojection3D_Kernel_Tex_Interp_Launcher(const float *sinogram_ptr, float *out, const float *projection_matrices, const int number_of_projections,
                                          const int volume_width, const int volume_height, const int volume_depth,
                                          const float volume_spacing_x, const float volume_spacing_y, const float volume_spacing_z,
                                          const float volume_origin_x, const float volume_origin_y, const float volume_origin_z,
                                          const int detector_width, const int detector_height, const float projection_multiplier)
{
    //COPY matrix to graphics card as float array
    auto matrices_size_b = number_of_projections * 12 * sizeof(float);
    float *d_projection_matrices;
    gpuErrchk(cudaMalloc(&d_projection_matrices, matrices_size_b));
    gpuErrchk(cudaMemcpy(d_projection_matrices, projection_matrices, matrices_size_b, cudaMemcpyHostToDevice));

    uint3 volume_size = make_uint3(volume_width, volume_height, volume_depth);
    float3 volume_spacing = make_float3(volume_spacing_x, volume_spacing_y, volume_spacing_z);
    float3 volume_origin = make_float3(volume_origin_x, volume_origin_y, volume_origin_z);

    uint2 detector_size = make_uint2(detector_width, detector_height);

    //COPY volume to graphics card

    // set texture properties
    sinogram_as_texture.addressMode[0] = cudaAddressModeBorder;
    sinogram_as_texture.addressMode[1] = cudaAddressModeBorder;
    sinogram_as_texture.addressMode[2] = cudaAddressModeBorder;
    sinogram_as_texture.filterMode = cudaFilterModeLinear;
    sinogram_as_texture.normalized = false;

    // malloc cuda array for texture
    cudaExtent projExtent = make_cudaExtent( detector_size.x,
                                             detector_size.y,
                                             number_of_projections );
    
    cudaArray *projArray;
    
    static cudaChannelFormatDesc channelDesc = cudaCreateChannelDesc<float>();
    gpuErrchk( cudaMalloc3DArray( &projArray, &channelDesc, projExtent, cudaArrayLayered ) );

    auto pitch_ptr = make_cudaPitchedPtr( const_cast<float*>( sinogram_ptr ),
                                                detector_size.x*sizeof(float),
                                                detector_size.x,
                                                detector_size.y
                                            );
    // copy data to 3D array
    cudaMemcpy3DParms copyParams = {0};
    copyParams.srcPtr   = pitch_ptr;
    copyParams.dstArray = projArray;
    copyParams.extent   = projExtent;
    copyParams.kind     = cudaMemcpyDeviceToDevice;
    gpuErrchk( cudaMemcpy3D( &copyParams ) );

    // bind texture reference
    gpuErrchk( cudaBindTextureToArray( sinogram_as_texture, projArray, channelDesc ) );

    // launch kernel
    const unsigned int gridsize_x = (volume_size.x-1) / BLOCKSIZE_X + 1;
    const unsigned int gridsize_y = (volume_size.y-1) / BLOCKSIZE_Y + 1;
    const unsigned int gridsize_z = (volume_size.z-1) / BLOCKSIZE_Z + 1;
    const dim3 grid = dim3( gridsize_x, gridsize_y, gridsize_z );
    const dim3 block = dim3( BLOCKSIZE_X, BLOCKSIZE_Y, BLOCKSIZE_Z );

    backproject_3Dcone_beam_kernel_tex_interp<<< grid, block >>>( out, d_projection_matrices, number_of_projections,
                                                            volume_size, volume_spacing, volume_origin, projection_multiplier );


    gpuErrchk(cudaUnbindTexture(sinogram_as_texture));
    gpuErrchk(cudaFreeArray(projArray));
    gpuErrchk(cudaFree(d_projection_matrices));
}

#endif


/*
 * Voxel-driven cone-beam back-projector CUDA kernel using hardware interpolation
 * Implementation adapted from CONRAD
 * PyRo-ML is developed as an Open Source project under the GNU General Public License (GPL).
*/