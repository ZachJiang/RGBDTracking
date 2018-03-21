/*
 * cudaSegmentation.cpp
 *
 *  Created on: Apr 24, 2014
 *      Author: apetit
 */

/*
 * Copyright 1993-2014 NVIDIA Corporation.  All rights reserved.
 *
 * Please refer to the NVIDIA end user license agreement (EULA) associated
 * with this source code for terms and conditions that govern your use of
 * this software. Any use, reproduction, disclosure, or distribution of
 * this software and related documentation outside the terms of the EULA
 * is strictly prohibited.
 *
 */

#include <stdio.h>
#include <math.h>
#include <string.h>
#include <cstdio>
#include <stdlib.h>
#include <vector>

#include <opencv/cv.h>
#include <opencv2/core.hpp>
#include <opencv2/features2d.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/highgui.hpp>

#include "FreeImage.h"

#include <npp.h>          // CUDA NPP Definitions
#include <helper_cuda.h>  // helper for CUDA Error handling and initialization

#include "cudaSegmentation.h"
#include <Exceptions.h>   // Found in common/UtilNPP

using namespace cv;
using namespace std;


// Parameters

#define DOWNSAMPLE_FIRST
//#define PREFILTER

//NOTE: Alternativ color model to GMM model proposed in the orginial paper.
//#define USE_HISTOGRAM_MODEL

#define CLUSTER_ALWAYS
#define COLOR_CLUSTER 2

#define MAX_ITERATIONS 0
#define EDGE_STRENGTH 50.0f


// Functions from GrabcutGMM.cu
cudaError_t GMMAssign(int gmmN, const float *gmm, int gmm_pitch, const uchar4 *image, int image_pitch, unsigned char *alpha, int alpha_pitch, int width, int height);
cudaError_t GMMInitialize(int gmm_N, float *gmm, float *scratch_mem, int gmm_pitch, const uchar4 *image, int image_pitch, unsigned char *alpha, int alpha_pitch, int width, int height);
cudaError_t GMMUpdate(int gmm_N, float *gmm, float *scratch_mem, int gmm_pitch, const uchar4 *image, int image_pitch, unsigned char *alpha, int alpha_pitch, int width, int height);
cudaError_t GMMDataTerm(Npp32s *terminals, int terminal_pitch, int gmmN, const float *gmm, int gmm_pitch, const uchar4 *image, int image_pitch, const unsigned char *trimap, int trimap_pitch, int width, int height);

cudaError_t HistogramUpdate(int *histogram, int *histogram_temp, const uchar4 *image, int image_pitch, const unsigned char *trimap, int trimap_pitch, int width, int height);
cudaError_t HistogramDataTerm(Npp32s *terminals, int terminal_pitch, int *histogram, float *loglikelihood, const uchar4 *image, int image_pitch, const unsigned char *trimap, int trimap_pitch, int width, int height);
int HistogramGetScratchSize();

cudaError_t EdgeCues(float alpha, const uchar4 *image, int image_pitch, Npp32s *left_transposed, Npp32s *right_transposed, Npp32s *top, Npp32s *bottom, Npp32s *topleft, Npp32s *topright, Npp32s *bottomleft, Npp32s *bottomright, int pitch, int transposed_pitch, int width, int height, float *scratch_mem);
cudaError_t downscale(uchar4 *small_image, int small_pitch, int small_width, int small_height, const uchar4 *image, int pitch, int width, int height);
cudaError_t downscaleTrimap(unsigned char *small_image, int small_pitch, int small_width, int small_height, const unsigned char *image, int pitch, int width, int height);
cudaError_t upsampleAlpha(unsigned char *alpha, unsigned char *small_alpha, int alpha_pitch, int width, int height, int small_width, int small_height);
cudaError_t SegmentationChanged(bool &result, int *d_changed, Npp8u *alpha_old, Npp8u *alpha_new, int alpha_pitch, int width, int height);


cudaSegmentation::cudaSegmentation(const uchar4 *image, int _image_pitch, unsigned char *trimap, int _trimap_pitch, int width, int height) : d_trimap(trimap), trimap_pitch(_trimap_pitch), d_crop_trimap(trimap), crop_trimap_pitch(_trimap_pitch)
{

    //updateImageCrop(image, _image_pitch, width, height);
    size.width = width;
    size.height = height;
    gmms = 2 * COLOR_CLUSTER;
    edge_strength = EDGE_STRENGTH;

    m_neighborhood = 8;

    blocks = ((width+31)/32) * ((height+31)/32);
    gmm_pitch = 11 * sizeof(float);

    checkCudaErrors(cudaMallocPitch(&d_image, &image_pitch, width * 4, height));
    image_pitch = _image_pitch;
    cudaMemcpy(d_image, image, image_pitch * height, cudaMemcpyDeviceToDevice);

    // Doublebuffered alpha
    checkCudaErrors(cudaMallocPitch(&d_alpha[0], &alpha_pitch, width, height));
    checkCudaErrors(cudaMallocPitch(&d_alpha[1], &alpha_pitch, width, height));

    // Graph
    checkCudaErrors(cudaMallocPitch(&d_terminals, &pitch, width*sizeof(Npp32f), height));
    checkCudaErrors(cudaMallocPitch(&d_top, &pitch, width*sizeof(Npp32f), height));
    checkCudaErrors(cudaMallocPitch(&d_topleft, &pitch, width*sizeof(Npp32f), height));
    checkCudaErrors(cudaMallocPitch(&d_topright, &pitch, width*sizeof(Npp32f), height));
    checkCudaErrors(cudaMallocPitch(&d_bottom, &pitch, width*sizeof(Npp32f), height));
    checkCudaErrors(cudaMallocPitch(&d_bottomleft, &pitch, width*sizeof(Npp32f), height));
    checkCudaErrors(cudaMallocPitch(&d_bottomright, &pitch, width*sizeof(Npp32f), height));
    checkCudaErrors(cudaMallocPitch(&d_left_transposed, &transposed_pitch, height*sizeof(Npp32s), width));
    checkCudaErrors(cudaMallocPitch(&d_right_transposed, &transposed_pitch, height*sizeof(Npp32s), width));


    int scratch_gc_size;
    nppiGraphcut8GetSize(size, &scratch_gc_size);

    int scratch_gmm_size = (int)(blocks * gmm_pitch * gmms + blocks * 4);

    int scratch_histogram_size = HistogramGetScratchSize();

    checkCudaErrors(cudaMalloc(&d_scratch_mem, MAX(scratch_histogram_size, MAX(scratch_gmm_size, scratch_gc_size))));

    NPP_CHECK_NPP(nppiGraphcutInitAlloc(size, &pState, d_scratch_mem));

    checkCudaErrors(cudaMalloc(&d_gmm, gmm_pitch * gmms));
    checkCudaErrors(cudaMalloc(&d_histogram, 4 * 65537));


#ifdef DOWNSAMPLE_FIRST
    // Estimate color models on lower res input image first
    createSmallImage(MAX(width/4, height/4));
#endif

    checkCudaErrors(cudaEventCreate(&start));
    checkCudaErrors(cudaEventCreate(&stop));
};

cudaSegmentation::~cudaSegmentation()
{
    checkCudaErrors(cudaFree(d_image));
    checkCudaErrors(cudaFree(d_alpha[0]));
    checkCudaErrors(cudaFree(d_alpha[1]));
    checkCudaErrors(cudaFree(d_terminals));
    checkCudaErrors(cudaFree(d_top));
    checkCudaErrors(cudaFree(d_bottom));
    checkCudaErrors(cudaFree(d_topleft));
    checkCudaErrors(cudaFree(d_topright));
    checkCudaErrors(cudaFree(d_bottomleft));
    checkCudaErrors(cudaFree(d_bottomright));
    checkCudaErrors(cudaFree(d_left_transposed));
    checkCudaErrors(cudaFree(d_right_transposed));
    checkCudaErrors(cudaFree(d_scratch_mem));
    checkCudaErrors(cudaFree(d_gmm));
    checkCudaErrors(cudaFree(d_histogram));
    nppiGraphcutFree(pState);

    checkCudaErrors(cudaFree(d_crop_image));
    checkCudaErrors(cudaFree(d_crop_alpha[0]));
    checkCudaErrors(cudaFree(d_crop_alpha[1]));
    checkCudaErrors(cudaFree(d_crop_terminals));
    checkCudaErrors(cudaFree(d_crop_top));
    checkCudaErrors(cudaFree(d_crop_bottom));
    checkCudaErrors(cudaFree(d_crop_topleft));
    checkCudaErrors(cudaFree(d_crop_topright));
    checkCudaErrors(cudaFree(d_crop_bottomleft));
    checkCudaErrors(cudaFree(d_crop_bottomright));
    checkCudaErrors(cudaFree(d_crop_left_transposed));
    checkCudaErrors(cudaFree(d_crop_right_transposed));
    checkCudaErrors(cudaFree(d_crop_scratch_mem));
    nppiGraphcutFree(crop_pState);

#ifdef DOWNSAMPLE_FIRST
    checkCudaErrors(cudaFree(d_small_image));
#endif

    checkCudaErrors(cudaEventDestroy(start));
    checkCudaErrors(cudaEventDestroy(stop));
};

void cudaSegmentation::updateImage(const uchar4 *image)
{

std::cout << " size 0 " << size.width << std::endl;
    checkCudaErrors(cudaMemcpy(d_image, image, image_pitch * size.height, cudaMemcpyDeviceToDevice));
}

void cudaSegmentation::updateImageCrop(const uchar4 *crop_image, int _crop_image_pitch, int crop_width, int crop_height)
{
    crop_size.width = crop_width;
    crop_size.height = crop_height;
	
    crop_blocks = ((crop_width+31)/32) * ((crop_height+31)/32);

    checkCudaErrors(cudaMallocPitch(&d_crop_image, &crop_image_pitch, crop_width * 4, crop_height));
    crop_image_pitch = _crop_image_pitch;
    cudaMemcpy(d_crop_image, crop_image, crop_image_pitch * crop_height, cudaMemcpyDeviceToDevice);

    // Doublebuffered alpha
    checkCudaErrors(cudaMallocPitch(&d_crop_alpha[0], &crop_alpha_pitch, crop_width, crop_height));
    checkCudaErrors(cudaMallocPitch(&d_crop_alpha[1], &crop_alpha_pitch, crop_width, crop_height));
	

    // Graph
    checkCudaErrors(cudaMallocPitch(&d_crop_terminals, &crop_pitch, crop_width*sizeof(Npp32f), crop_height));
    checkCudaErrors(cudaMallocPitch(&d_crop_top, &crop_pitch, crop_width*sizeof(Npp32f), crop_height));
    checkCudaErrors(cudaMallocPitch(&d_crop_topleft, &crop_pitch, crop_width*sizeof(Npp32f), crop_height));
    checkCudaErrors(cudaMallocPitch(&d_crop_topright, &crop_pitch, crop_width*sizeof(Npp32f), crop_height));
    checkCudaErrors(cudaMallocPitch(&d_crop_bottom, &crop_pitch, crop_width*sizeof(Npp32f), crop_height));
    checkCudaErrors(cudaMallocPitch(&d_crop_bottomleft, &crop_pitch, crop_width*sizeof(Npp32f), crop_height));
    checkCudaErrors(cudaMallocPitch(&d_crop_bottomright, &crop_pitch, crop_width*sizeof(Npp32f), crop_height));
    checkCudaErrors(cudaMallocPitch(&d_crop_left_transposed, &crop_transposed_pitch, crop_height*sizeof(Npp32s), crop_width));
    checkCudaErrors(cudaMallocPitch(&d_crop_right_transposed, &crop_transposed_pitch, crop_height*sizeof(Npp32s), crop_width));

    
	int scratch_gc_size;
    nppiGraphcut8GetSize(crop_size, &scratch_gc_size);

    int scratch_gmm_size = (int)(crop_blocks * gmm_pitch * gmms + crop_blocks * 4);

    int scratch_histogram_size = HistogramGetScratchSize();

    checkCudaErrors(cudaMalloc(&d_crop_scratch_mem, MAX(scratch_histogram_size, MAX(scratch_gmm_size, scratch_gc_size))));

    NPP_CHECK_NPP(nppiGraphcutInitAlloc(crop_size, &crop_pState, d_crop_scratch_mem));

    //checkCudaErrors(cudaMalloc(&d_gmm, gmm_pitch * gmms));
    //checkCudaErrors(cudaMalloc(&d_histogram, 4 * 65537));


/*#ifdef DOWNSAMPLE_FIRST
    // Estimate color models on lower res input image first
    createSmallImage(MAX(width/4, height/4));
#endif

    checkCudaErrors(cudaEventCreate(&start));
    checkCudaErrors(cudaEventCreate(&stop));*/
}

void cudaSegmentation::updateTrimap(uchar *trimap)
{
	d_trimap = trimap;
    //checkCudaErrors(cudaMemcpy(d_trimap, trimap, trimap_pitch * size.height, cudaMemcpyDeviceToDevice));
}

void cudaSegmentation::updateTrimapCrop(uchar *trimap, uchar *crop_trimap, int _crop_trimap_pitch)
{
	d_trimap = trimap;
	d_crop_trimap = crop_trimap;
	crop_trimap_pitch = _crop_trimap_pitch;
    //checkCudaErrors(cudaMemcpy(d_trimap, trimap, trimap_pitch * size.height, cudaMemcpyDeviceToDevice));
}


void cudaSegmentation::computeSegmentationFromTrimap()
{
    int iteration=0;
    current_alpha = 0;

    double t = (double)getTickCount();

    checkCudaErrors(cudaEventRecord(start,0));

#ifdef DOWNSAMPLE_FIRST
    // Solve Grabcut on lower resolution first. Reduces total computation time.
    createSmallTrimap();

    checkCudaErrors(cudaMemcpy2DAsync(d_alpha[0], alpha_pitch, d_small_trimap[small_trimap_idx], small_trimap_pitch[small_trimap_idx], small_size.width, small_size.height, cudaMemcpyDeviceToDevice));

    for (int i=0; i<2; ++i)
    {
#ifdef USE_HISTOGRAM_MODEL
        checkCudaErrors(HistogramUpdate(d_histogram, (int *)d_scratch_mem, d_small_image, (int)small_pitch, d_alpha[current_alpha], (int)alpha_pitch, small_size.width, small_size.height));
        checkCudaErrors(HistogramDataTerm(d_terminals, (int)pitch, d_histogram, (float *)d_scratch_mem,  d_small_image, (int)small_pitch, d_small_trimap[small_trimap_idx], (int)small_trimap_pitch[small_trimap_idx], small_size.width, small_size.height));
#else
        checkCudaErrors(GMMInitialize(gmms, d_gmm, (float *)d_scratch_mem, (int)gmm_pitch, d_small_image, (int)small_pitch, d_alpha[current_alpha], (int)alpha_pitch, small_size.width, small_size.height));
        checkCudaErrors(GMMUpdate(gmms, d_gmm, (float *)d_scratch_mem, (int)gmm_pitch, d_small_image, (int)small_pitch, d_alpha[current_alpha], (int)alpha_pitch, small_size.width, small_size.height));
        checkCudaErrors(GMMDataTerm(d_terminals, (int)pitch, gmms, d_gmm, (int)gmm_pitch, d_small_image, (int)small_pitch, d_small_trimap[small_trimap_idx], (int)small_trimap_pitch[small_trimap_idx], small_size.width, small_size.height));
#endif
        checkCudaErrors(EdgeCues(edge_strength, d_small_image, (int)small_pitch, d_left_transposed, d_right_transposed, d_top, d_bottom, d_topleft, d_topright, d_bottomleft, d_bottomright, (int)pitch, (int)transposed_pitch, small_size.width, small_size.height, (float *) d_scratch_mem));

        NPP_CHECK_NPP(nppiGraphcut_32s8u(d_terminals, d_left_transposed, d_right_transposed, d_top, d_bottom, (int)pitch, (int)transposed_pitch, small_size, d_alpha[1-current_alpha],
                                         (int)alpha_pitch, pState));

        // Map graphcut result to 0/1
        NPP_CHECK_NPP(nppiThreshold_8u_C1IR(d_alpha[1-current_alpha], (int)alpha_pitch, small_size, 1, NPP_CMP_GREATER));

        current_alpha = 1-current_alpha;
    }

    checkCudaErrors(upsampleAlpha(d_alpha[1-current_alpha], d_alpha[current_alpha], (int)alpha_pitch, size.width, size.height, small_size.width, small_size.height));
    current_alpha = 1-current_alpha;


#else
    cudaMemcpy2DAsync(d_alpha[0], alpha_pitch, d_trimap, trimap_pitch, alpha_pitch, size.height, cudaMemcpyDeviceToDevice);
#endif

#ifdef USE_HISTOGRAM_MODEL
    checkCudaErrors(HistogramUpdate(d_histogram, (int *)d_scratch_mem, d_image, (int)image_pitch, d_alpha[current_alpha], (int)alpha_pitch, size.width, size.height));
#else
    checkCudaErrors(GMMInitialize(gmms, d_gmm, (float *)d_scratch_mem, (int)gmm_pitch, d_image, (int)image_pitch, d_alpha[current_alpha], (int)alpha_pitch, size.width, size.height));
    checkCudaErrors(GMMUpdate(gmms, d_gmm, (float *)d_scratch_mem, (int)gmm_pitch, d_image, (int)image_pitch, d_alpha[current_alpha],(int)alpha_pitch, size.width, size.height));
#endif

    t = ((double)getTickCount() - t)/getTickFrequency();
    cout << "Times passed in seconds 0: " << t << endl;

    while (iteration<1)
    {

#ifdef USE_HISTOGRAM_MODEL
        checkCudaErrors(HistogramDataTerm(d_terminals, (int)pitch, d_histogram, (float *)d_scratch_mem, d_image, (int)image_pitch, d_trimap, (int)trimap_pitch, size.width, size.height));
#else
        checkCudaErrors(GMMDataTerm(d_terminals, (int)pitch, gmms, d_gmm, (int)gmm_pitch, d_image, (int)image_pitch, d_trimap, (int)trimap_pitch, size.width, size.height));
#endif

        checkCudaErrors(EdgeCues(edge_strength, d_image, (int)image_pitch, d_left_transposed, d_right_transposed, d_top, d_bottom, d_topleft, d_topright, d_bottomleft, d_bottomright,(int) pitch, (int)transposed_pitch, size.width, size.height, (float *) d_scratch_mem));

        current_alpha = 1 ^ current_alpha;

        t = (double)getTickCount();

        if (m_neighborhood == 8)
        {
            NPP_CHECK_NPP(nppiGraphcut8_32s8u(d_terminals, d_left_transposed, d_right_transposed, d_top, d_topleft, d_topright, d_bottom, d_bottomleft, d_bottomright, (int)pitch, (int)transposed_pitch, size, d_alpha[current_alpha],
                                              (int)alpha_pitch, pState));
        }
        else
        {

            NPP_CHECK_NPP(nppiGraphcut_32s8u(d_terminals, d_left_transposed, d_right_transposed, d_top, d_bottom,(int)pitch, (int)transposed_pitch, size, d_alpha[current_alpha],
                                             (int)alpha_pitch, pState));
        }
        t = ((double)getTickCount() - t)/getTickFrequency();
        cout << "Times passed in seconds 1: " << t << endl;

        t = (double)getTickCount();

        // Map graphcut result to 0/1
        NPP_CHECK_NPP(nppiThreshold_8u_C1IR(d_alpha[current_alpha], (int)alpha_pitch, size, 1, NPP_CMP_GREATER));

        if (iteration > 0)
        {
            bool changed;
            checkCudaErrors(SegmentationChanged(changed, (int *)d_scratch_mem, d_alpha[1-current_alpha], d_alpha[current_alpha], (int)alpha_pitch, size.width, size.height));

            // Solution has converged
            if (!changed)
            {
                break;
            }
        }

        if (iteration > MAX_ITERATIONS)
        {
            // Does not converge, fallback to rect selection
            printf("Warning: Color models did not converge after %d iterations.\n", MAX_ITERATIONS);
            break;
        }

        t = ((double)getTickCount() - t)/getTickFrequency();
        cout << "Times passed in seconds 2: " << t << endl;

        t = (double)getTickCount();

#ifdef USE_HISTOGRAM_MODEL
        checkCudaErrors(HistogramUpdate(d_histogram, (int *)d_scratch_mem, d_image, (int)image_pitch, d_alpha[current_alpha], (int)alpha_pitch, size.width, size.height));
#else
#ifdef CLUSTER_ALWAYS
        checkCudaErrors(GMMInitialize(gmms, d_gmm, (float *)d_scratch_mem, (int)gmm_pitch, d_image, (int)image_pitch, d_alpha[current_alpha], (int)alpha_pitch, size.width, size.height));
#else
        checkCudaErrors(GMMAssign(gmms, d_gmm, gmm_pitch, d_image, image_pitch, d_alpha[current_alpha], alpha_pitch, size.width, size.height));
#endif
        checkCudaErrors(GMMUpdate(gmms, d_gmm, (float *) d_scratch_mem,(int) gmm_pitch, d_image, (int)image_pitch, d_alpha[current_alpha], (int)alpha_pitch, size.width, size.height));
#endif

        t = ((double)getTickCount() - t)/getTickFrequency();
        cout << "Times passed in seconds 3: " << t << endl;

        iteration++;
    }

    checkCudaErrors(cudaEventRecord(stop, 0));

    checkCudaErrors(cudaEventSynchronize(stop));
    float time;
    checkCudaErrors(cudaEventElapsedTime(&time, start, stop));

    printf("Neighborhood : %d\n", m_neighborhood);
    printf("Iterations   : %d\n", iteration);
    printf("Elapsed Time : %f ms\n\n", time);
}

void cudaSegmentation::updateSegmentation()
{

    checkCudaErrors(EdgeCues(edge_strength, d_image, (int)image_pitch, d_left_transposed, d_right_transposed, d_top, d_bottom, d_topleft, d_topright, d_bottomleft, d_bottomright, (int)pitch, (int)transposed_pitch, size.width, size.height, (float *) d_scratch_mem));
#ifdef USE_HISTOGRAM_MODEL
    checkCudaErrors(HistogramUpdate(d_histogram, (int *)d_scratch_mem, d_image, (int)image_pitch, d_alpha[current_alpha], (int)alpha_pitch, size.width, size.height));
    checkCudaErrors(HistogramDataTerm(d_terminals, (int)pitch, d_histogram, (float *)d_scratch_mem, d_image, (int)image_pitch, d_trimap, (int)trimap_pitch, size.width, size.height));
#else
    //checkCudaErrors(GMMInitialize(gmms, d_gmm, (float *)d_scratch_mem, (int)gmm_pitch, d_image, (int)image_pitch, d_alpha[current_alpha], (int)alpha_pitch, size.width, size.height));
    //checkCudaErrors(GMMUpdate(gmms, d_gmm, (float *)d_scratch_mem, (int)gmm_pitch, d_small_image, (int)small_pitch, d_alpha[current_alpha], (int)alpha_pitch, small_size.width, small_size.height));
    checkCudaErrors(GMMDataTerm(d_terminals, (int)pitch, gmms, d_gmm, (int)gmm_pitch, d_image, (int)image_pitch, d_trimap, (int)trimap_pitch, size.width, size.height));
#endif

    double t = (double)getTickCount();


    if (m_neighborhood == 8)
    {
        NPP_CHECK_NPP(nppiGraphcut8_32s8u(d_terminals, d_left_transposed, d_right_transposed, d_top, d_topleft, d_topright, d_bottom, d_bottomleft, d_bottomright, (int)pitch,(int) transposed_pitch, size, d_alpha[current_alpha],
                                          (int)alpha_pitch, pState));
    }
    else
    {
        NPP_CHECK_NPP(nppiGraphcut_32s8u(d_terminals, d_left_transposed, d_right_transposed, d_top,d_bottom, (int)pitch,(int) transposed_pitch, size, d_alpha[current_alpha],
                                         (int)alpha_pitch, pState));
    }

    t = ((double)getTickCount() - t)/getTickFrequency();
    cout << "Times passed in seconds 1: " << t << endl;

    // Map graphcut result to 0/1
    NPP_CHECK_NPP(nppiThreshold_8u_C1IR(d_alpha[current_alpha], (int)alpha_pitch, size, 1, NPP_CMP_GREATER));
}

void cudaSegmentation::updateSegmentationCrop()
{
	
    cudaMemcpy2DAsync(d_crop_alpha[0], crop_alpha_pitch, d_crop_trimap, crop_trimap_pitch, crop_alpha_pitch, crop_size.height, cudaMemcpyDeviceToDevice);

   std::cout << " crop pitch " << (int)crop_trimap_pitch << " " << (int)crop_image_pitch << " " << crop_size.width << " " << crop_size.height << std::endl;
   //crop_pitch = crop_image_pitch;
    checkCudaErrors(EdgeCues(edge_strength, d_crop_image, (int)crop_image_pitch, d_crop_left_transposed, d_crop_right_transposed, d_crop_top, d_crop_bottom, d_crop_topleft, d_crop_topright, d_crop_bottomleft, d_crop_bottomright, (int)crop_pitch, (int)crop_transposed_pitch, crop_size.width, crop_size.height, (float *) d_crop_scratch_mem));
#ifdef USE_HISTOGRAM_MODEL
    checkCudaErrors(HistogramUpdate(d_histogram, (int *)d_scratch_mem, d_image, (int)image_pitch, d_alpha[current_alpha], (int)alpha_pitch, size.width, size.height));
    checkCudaErrors(HistogramDataTerm(d_crop_terminals, (int)crop_pitch, d_histogram, (float *)d_crop_scratch_mem, d_crop_image, (int)crop_image_pitch, d_crop_trimap, (int)crop_trimap_pitch, crop_size.width, crop_size.height));
#else
    //checkCudaErrors(GMMInitialize(gmms, d_gmm, (float *)d_scratch_mem, (int)gmm_pitch, d_image, (int)image_pitch, d_alpha[current_alpha], (int)alpha_pitch, size.width, size.height));
    //checkCudaErrors(GMMUpdate(gmms, d_gmm, (float *)d_scratch_mem, (int)gmm_pitch, d_small_image, (int)small_pitch, d_alpha[current_alpha], (int)alpha_pitch, small_size.width, small_size.height));
    checkCudaErrors(GMMDataTerm(d_crop_terminals, (int)crop_pitch, gmms, d_gmm, (int)gmm_pitch, d_crop_image, (int)crop_image_pitch, d_crop_trimap, (int)crop_trimap_pitch, crop_size.width, crop_size.height));
#endif

    double t = (double)getTickCount();
    current_alpha = 1;

    if (m_neighborhood == 8)
    {
        NPP_CHECK_NPP(nppiGraphcut8_32s8u(d_crop_terminals, d_crop_left_transposed, d_crop_right_transposed, d_crop_top, d_crop_topleft, d_crop_topright, d_crop_bottom, d_crop_bottomleft, d_crop_bottomright, (int)crop_pitch,(int) crop_transposed_pitch, crop_size, d_crop_alpha[current_alpha],
                                          (int)crop_alpha_pitch, crop_pState));
    }
    else
    {
        NPP_CHECK_NPP(nppiGraphcut_32s8u(d_crop_terminals, d_crop_left_transposed, d_crop_right_transposed, d_crop_top,d_crop_bottom, (int)crop_pitch,(int) crop_transposed_pitch, crop_size, d_crop_alpha[current_alpha],
                                         (int)crop_alpha_pitch, crop_pState));
    }

    t = ((double)getTickCount() - t)/getTickFrequency();
    cout << "Times passed in seconds 1: " << t << endl;

    // Map graphcut result to 0/1
    NPP_CHECK_NPP(nppiThreshold_8u_C1IR(d_crop_alpha[current_alpha], (int)crop_alpha_pitch, crop_size, 1, NPP_CMP_GREATER));

	checkCudaErrors(cudaFree(d_crop_image));
    checkCudaErrors(cudaFree(d_crop_terminals));
    checkCudaErrors(cudaFree(d_crop_top));
    checkCudaErrors(cudaFree(d_crop_bottom));
    checkCudaErrors(cudaFree(d_crop_topleft));
    checkCudaErrors(cudaFree(d_crop_topright));
    checkCudaErrors(cudaFree(d_crop_bottomleft));
    checkCudaErrors(cudaFree(d_crop_bottomright));
    checkCudaErrors(cudaFree(d_crop_left_transposed));
    checkCudaErrors(cudaFree(d_crop_right_transposed));
    checkCudaErrors(cudaFree(d_crop_scratch_mem));

}

void cudaSegmentation::createSmallImage(int max_dim)
{
    int temp_width[2];
    int temp_height[2];

    uchar4 *d_temp[2];
    size_t temp_pitch[2];

    temp_width[0] = (int)ceil(size.width * 0.5f);
    temp_height[0] = (int)ceil(size.height* 0.5f);

    temp_width[1] = (int)ceil(temp_width[0] * 0.5f);
    temp_height[1] = (int)ceil(temp_height[0] * 0.5f);

    checkCudaErrors(cudaMallocPitch(&d_temp[0], &temp_pitch[0], temp_width[0] * 4, temp_height[0]));
    checkCudaErrors(cudaMallocPitch(&d_temp[1], &temp_pitch[1], temp_width[1] * 4, temp_height[1]));

    // Alloc also the small trimaps
    checkCudaErrors(cudaMallocPitch(&d_small_trimap[0], &small_trimap_pitch[0], temp_width[0], temp_height[0]));
    checkCudaErrors(cudaMallocPitch(&d_small_trimap[1], &small_trimap_pitch[1], temp_width[1], temp_height[1]));


    checkCudaErrors(downscale(d_temp[0], (int)temp_pitch[0], temp_width[0], temp_height[0], d_image, (int)image_pitch, size.width, size.height));
    int current = 0;

    while (temp_width[current] > max_dim || temp_height[current] > max_dim)
    {
        checkCudaErrors(downscale(d_temp[1-current], (int)temp_pitch[1-current], temp_width[1-current], temp_height[1-current], d_temp[current], (int)temp_pitch[current], temp_width[current], temp_height[current]));
        current ^= 1;
        temp_width[1-current] = (int)ceil(temp_width[current] * 0.5f);
        temp_height[1-current] = (int)ceil(temp_height[current] * 0.5f);
    }

    d_small_image = d_temp[current];
    small_pitch = temp_pitch[current];
    small_size.width = temp_width[current];
    small_size.height = temp_height[current];

    checkCudaErrors(cudaFree(d_temp[1-current]));
}

void cudaSegmentation::createSmallTrimap()
{
    int temp_width[2];
    int temp_height[2];

    temp_width[0] = (int)ceil(size.width * 0.5f);
    temp_height[0] = (int)ceil(size.height* 0.5f);

    temp_width[1] = (int)ceil(temp_width[0] * 0.5f);
    temp_height[1] = (int)ceil(temp_height[0] * 0.5f);

    checkCudaErrors(downscaleTrimap(d_small_trimap[0], (int)small_trimap_pitch[0], temp_width[0], temp_height[0], d_trimap, (int)trimap_pitch, size.width, size.height));

    small_trimap_idx = 0;

    while (temp_width[small_trimap_idx] != small_size.width)
    {
        checkCudaErrors(downscaleTrimap(d_small_trimap[1-small_trimap_idx], (int)small_trimap_pitch[1-small_trimap_idx], temp_width[1-small_trimap_idx], temp_height[1-small_trimap_idx], d_small_trimap[small_trimap_idx], (int)small_trimap_pitch[small_trimap_idx], temp_width[small_trimap_idx], temp_height[small_trimap_idx]));
        small_trimap_idx ^= 1;
        temp_width[1-small_trimap_idx] = (int)ceil(temp_width[small_trimap_idx] * 0.5f);
        temp_height[1-small_trimap_idx] = (int)ceil(temp_height[small_trimap_idx] * 0.5f);
    }
}
