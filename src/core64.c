#undef DT32
//#define DT32 //<- This should be the ONLY difference between core32 and core64!

#ifdef DT32
 #define flt float
 #define DT_CALC DT_FLOAT32
 #define epsilon FLT_EPSILON
#else
 #define flt double
 #define DT_CALC DT_FLOAT64
 #define epsilon DBL_EPSILON
#endif

#ifdef USING_R
#include "print.h"
#else
#include <stdio.h>
#include <stdlib.h>
#include <nifti2_io.h>
#endif
#include <float.h> //FLT_EPSILON
#ifdef __aarch64__
  #include "arm_malloc.h"
#else
  #include <immintrin.h>
#endif
#include <limits.h>
#include <math.h>

#if defined(_OPENMP)
	#include <omp.h>
#endif
#include "core.h"

#define bandpass
#ifdef bandpass
#include "bw.h"
#endif

//#define slicetimer //tensor_decomp support is optional
#ifdef slicetimer
 #include "afni.h"
#endif 

#define tensor_decomp //tensor_decomp support is optional
#ifdef tensor_decomp
 #include "tensor.h"
#endif 

//#define TFCE //formerly we used Christian Gaser's tfce, new bespoke code handles connectivity
//#ifdef TFCE //we now use in-built tfce function
//	#include "tfce_pthread.h"
//#endif

static int show_helpx( void ) {
#ifdef USING_R
    Rf_error("Fatal: show_help shown by wrapper function");
#else
	niimath_print("Fatal: show_help shown by wrapper function\n");
	exit(1);
#endif
}

static flt vx(flt * f, int p, int q) {
	if ((f[p] == INFINITY) || (f[q] == INFINITY))
		return INFINITY;
	else
		return ((f[q] + q*q) - (f[p] + p*p)) / (2.0*q - 2.0*p);
}

static void edt(flt * f, int n) {
	int q, p, k;
	flt s, dx;
	flt * d = (flt *)_mm_malloc((n)*sizeof(flt), 64);
	flt * z = (flt *)_mm_malloc((n)*sizeof(flt), 64); 
	int * v = (int *)_mm_malloc((n)*sizeof(int), 64);
    /*# Find the lower envelope of a sequence of parabolas.
    #   f...source data (returns the Y of the parabola vertex at X)
    #   d...destination data (final distance values are written here)
    #   z...temporary used to store X coords of parabola intersections
    #   v...temporary used to store X coords of parabola vertices
    #   i...resulting X coords of parabola vertices
    #   n...number of pixels in "f" to process
    # Always add the first pixel to the enveloping set since it is
    # obviously lower than all parabolas processed so far.*/
    k = 0;
    v[0] = 0;
    z[0] = -INFINITY;
    z[1] = INFINITY;
    for (q = 1; q < n; q++ ) {
	    /* If the new parabola is lower than the right-most parabola in
        # the envelope, remove it from the envelope. To make this
        # determination, find the X coordinate of the intersection (s)
        # between the parabolas with vertices at (q,f[q]) and (p,f[p]).*/
        p = v[k];
        s = vx(f, p,q);
        while (s <= z[k]) {
            k = k - 1;
            p = v[k];
            s = vx(f, p,q);
        }
        //# Add the new parabola to the envelope.
        k = k + 1;
        v[k] = q;
        z[k] = s;
        z[k + 1] = INFINITY;
    }
    /*# Go back through the parabolas in the envelope and evaluate them
    # in order to populate the distance values at each X coordinate.*/
    k = 0;
    for (q = 0; q < n; q++ ) {
	    while (z[k + 1] < q)
            k = k + 1;
        dx = (q - v[k]);
        d[q] = dx * dx + f[v[k]];
    }
    for (q = 0; q < n; q++ )
		f[q] = d[q];
	_mm_free (d);
	_mm_free (z);
	_mm_free (v);
}

static void edt1(flt * df, int n) { //first dimension is simple
	int q, prevX;
	flt prevY, v;
	prevX = 0;
	prevY = INFINITY;
	//forward
	for (q = 0; q < n; q++ ) {
		if (df[q] == 0) { 
			prevX = q;
			prevY = 0;
		} else
			df[q] = sqr(q-prevX)+prevY;
	}
	//reverse
	prevX = n;
	prevY = INFINITY;
	for (q = (n-1); q >= 0; q-- ) {
		v = sqr(q-prevX)+prevY;
		if (df[q] < v) { 
        	prevX = q;
        	prevY = df[q];
    	} else        
        	df[q] = v;
    } 
}

static int nifti_edt(nifti_image * nim) {
//https://github.com/neurolabusc/DistanceFields
	if ((nim->nvox < 1) || (nim->nx < 2) || (nim->ny < 2) || (nim->nz < 1)) return 1;
	if (nim->datatype != DT_CALC) return 1;
	flt * img = (flt *) nim->data; 
	//int nVol = 1;
	//for (int i = 4; i < 8; i++ )
	//	nVol *= MAX(nim->dim[i],1);
	int nvox3D = nim->nx * nim->ny * MAX(nim->nz, 1);
	int nVol = nim->nvox / nvox3D;
	if ((nvox3D * nVol) != nim->nvox) return 1;
	int nx = nim->nx;
	int ny = nim->ny;
	int nz = nim->nz;
	flt threshold = 0.0;	
	for (size_t i = 0; i < nim->nvox; i++ ) {
		if (img[i] > threshold)
			img[i] = INFINITY;
		else
			img[i] = 0;
	}
	size_t nRow = 1;
	for (int i = 2; i < 8; i++ )
		nRow *= MAX(nim->dim[i],1);
	//EDT in left-right direction
	for (int r = 0; r < nRow; r++ ) {
		flt * imgRow = img + (r * nx);
		edt1(imgRow, nx);
	}
	//EDT in anterior-posterior direction
	nRow = nim->nx * nim->nz; //transpose XYZ to YXZ and blur Y columns with XZ Rows
	for (int v = 0; v < nVol; v++ ) { //transpose each volume separately
		flt * img3D = (flt *)_mm_malloc(nvox3D*sizeof(flt), 64); //alloc for each volume to allow openmp
		//transpose data
		size_t vo = v * nvox3D; //volume offset
		for (int z = 0; z < nz; z++ ) {
			int zo = z * nx * ny;
			for (int y = 0; y < ny; y++ ) {
				int xo = 0;
				for (int x = 0; x < nx; x++ ) {
					img3D[zo+xo+y] = img[vo];
					vo += 1;
					xo += ny;
				}
			}
		}
		//perform EDT for all rows
		for (int r = 0; r < nRow; r++ ) {
			flt * imgRow = img3D + (r * ny);
			edt(imgRow, ny);
		}
		//transpose data back
		vo = v * nvox3D; //volume offset
		for (int z = 0; z < nz; z++ ) {
			int zo = z * nx * ny;
			for (int y = 0; y < ny; y++ ) {
				int xo = 0;
				for (int x = 0; x < nx; x++ ) {
					img[vo] = img3D[zo+xo+y];
					vo += 1;
					xo += ny;
				}
			}
		}		
		_mm_free (img3D);
	} //for each volume
	//EDT in head-foot direction
	nRow = nim->nx * nim->ny; //transpose XYZ to ZXY and blur Z columns with XY Rows
	#pragma omp parallel for
	for (int v = 0; v < nVol; v++ ) { //transpose each volume separately
		flt * img3D = (flt *)_mm_malloc(nvox3D*sizeof(flt), 64); //alloc for each volume to allow openmp
		//transpose data
		size_t vo = v * nvox3D; //volume offset
		for (int z = 0; z < nz; z++ ) {
			for (int y = 0; y < ny; y++ ) {
				int yo = y * nz * nx;
				int xo = 0;
				for (int x = 0; x < nx; x++ ) {
					img3D[z+xo+yo] = img[vo];
					vo += 1;
					xo += nz;
				}
			}
		}
		//perform EDT for all "rows"
		for (int r = 0; r < nRow; r++ ) {
			flt * imgRow = img3D + (r * nz);
			edt(imgRow, nz);
		}
		//transpose data back
		vo = v * nvox3D; //volume offset
		for (int z = 0; z < nz; z++ ) {
			for (int y = 0; y < ny; y++ ) {
				int yo = y * nz * nx;
				int xo = 0;
				for (int x = 0; x < nx; x++ ) {
					img[vo] = img3D[z+xo+yo];
					vo += 1;
					xo += nz;
				} //x
			} //y
		} //z
		_mm_free (img3D);
	} //for each volume
	return 0;
}

//Gaussian blur, both serial and parallel variants, https://github.com/neurolabusc/niiSmooth
static void blurS(flt * img, int nx, int ny, flt xmm, flt Sigmamm) {
//serial blur 
	//make kernels
	if ((xmm == 0) || (nx < 2) || (ny < 1) || (Sigmamm <= 0.0)) return;
	//flt sigma = (FWHMmm/xmm)/sqrt(8*log(2)); 
	flt sigma = (Sigmamm/xmm); //mm to vox
	//round(6*sigma), ceil(4*sigma) seems spot on larger than fslmaths
	//int cutoffvox = round(6*sigma); //filter width to 6 sigma: faster but lower precision AFNI_BLUR_FIRFAC = 2.5
	int cutoffvox = ceil(4*sigma); //filter width to 6 sigma: faster but lower precision AFNI_BLUR_FIRFAC = 2.5
	//niimath_print(".Blur Cutoff (%g) %d\n", 4*sigma, cutoffvox);
	//validated on SPM12's 1.5mm isotropic mask_ICV.nii (discrete jump in number of non-zero voxels)
	//fslmaths  mask -s 2.26 f6.nii  //Blur Cutoff (6.02667) 7
	//fslmaths  mask -s 2.24 f4.nii  //Blur Cutoff (5.97333) 6
	cutoffvox = MAX(cutoffvox, 1);
	flt * k = (flt *)_mm_malloc((cutoffvox+1)*sizeof(flt), 64); //FIR Gaussian
	flt expd = 2*sigma*sigma;
	for (int i = 0; i <= cutoffvox; i++ )
		k[i] = exp(-1.0f*(i*i)/expd);
	//calculate start, end for each voxel in 
	int * kStart = (int *)_mm_malloc(nx*sizeof(int), 64); //-cutoff except left left columns, e.g. 0, -1, -2... cutoffvox
	int * kEnd = (int *)_mm_malloc(nx*sizeof(int), 64); //+cutoff except right columns
	flt * kWeight = (flt *)_mm_malloc(nx*sizeof(flt), 64); //ensure sum of kernel = 1.0
	for (int i = 0; i < nx; i++ ) {
		kStart[i] = MAX(-cutoffvox, -i);//do not read below 0
		kEnd[i] = MIN(cutoffvox, nx-i-1);//do not read beyond final columnn
		if ((i > 0) && (kStart[i] == (kStart[i-1])) && (kEnd[i] == (kEnd[i-1]))) { //reuse weight
				kWeight[i] = kWeight[i-1];
				continue;	
		 }
		 flt wt = 0.0f;
		 for (int j = kStart[i]; j <= kEnd[i]; j++ )
			wt += k[abs(j)];
		kWeight[i] = 1 / wt;
		//niimath_print("%d %d->%d %g\n", i, kStart[i], kEnd[i], kWeight[i]);
	}
	//apply kernel to each row
	flt * tmp = _mm_malloc(nx*sizeof(flt), 64); //input values prior to blur
	for (int y = 0; y < ny; y++ ) {
		//niimath_print("-+ %d:%d\n", y, ny);
		memcpy(tmp, img, nx*sizeof(flt));
		for (int x = 0; x < nx; x++ ) {
			flt sum = 0;
			for (int i = kStart[x]; i <= kEnd[x]; i++ ) 
				sum += tmp[x+i] * k[abs(i)];
			img[x] = sum * kWeight[x];
		}
		img += nx;
	} //blurX
	//free kernel
	_mm_free (tmp);
	_mm_free (k);
	_mm_free (kStart);
	_mm_free (kEnd);
	_mm_free (kWeight);
} 

#if defined(_OPENMP)

static void blurP(flt * img, int nx, int ny, flt xmm, flt FWHMmm) {
//parallel blur
	//make kernels
	if ((xmm == 0) || (nx < 2) || (ny < 1) || (FWHMmm <= 0.0)) return;
	//flt sigma = (FWHMmm/xmm)/sqrt(8*log(2)); 
	flt sigma = (FWHMmm/xmm); //mm to vox
	int cutoffvox = round(6*sigma); //filter width to 6 sigma: faster but lower precision AFNI_BLUR_FIRFAC = 2.5
	cutoffvox = MAX(cutoffvox, 1);
	flt * k = (flt *)_mm_malloc((cutoffvox+1)*sizeof(flt), 64); //FIR Gaussian
	flt expd = 2*sigma*sigma;
	for (int i = 0; i <= cutoffvox; i++ )
		k[i] = exp(-1.0f*(i*i)/expd);
	//calculate start, end for each voxel in 
	int * kStart = (int *)_mm_malloc(nx*sizeof(int), 64); //-cutoff except left left columns, e.g. 0, -1, -2... cutoffvox
	int * kEnd = (int *)_mm_malloc(nx*sizeof(int), 64); //+cutoff except right columns
	flt * kWeight = (flt *)_mm_malloc(nx*sizeof(flt), 64); //ensure sum of kernel = 1.0
	for (int i = 0; i < nx; i++ ) {
		kStart[i] = MAX(-cutoffvox, -i);//do not read below 0
		kEnd[i] = MIN(cutoffvox, nx-i-1);//do not read beyond final columnn
		if ((i > 0) && (kStart[i] == (kStart[i-1])) && (kEnd[i] == (kEnd[i-1]))) { //reuse weight
				kWeight[i] = kWeight[i-1];
				continue;	
		 }
		 flt wt = 0.0f;
		 for (int j = kStart[i]; j <= kEnd[i]; j++ )
			wt += k[abs(j)];
		kWeight[i] = 1 / wt;
		//niimath_print("%d %d->%d %g\n", i, kStart[i], kEnd[i], kWeight[i]);
	}
	//apply kernel to each row
	#pragma omp parallel for
	for (int y = 0; y < ny; y++ ) {
		flt * tmp = _mm_malloc(nx*sizeof(flt), 64); //input values prior to blur
		flt * imgx = img;
		imgx += (nx * y);
		memcpy(tmp, imgx, nx*sizeof(flt));
		for (int x = 0; x < nx; x++ ) {
			flt sum = 0;
			for (int i = kStart[x]; i <= kEnd[x]; i++ ) 
				sum += tmp[x+i] * k[abs(i)];
			imgx[x] = sum * kWeight[x];
		}
		_mm_free (tmp);	
	}
	//free kernel
	_mm_free (k);
	_mm_free (kStart);
	_mm_free (kEnd);
	_mm_free (kWeight);
} //blurP

#endif

static int nifti_smooth_gauss(nifti_image * nim, flt SigmammX, flt SigmammY, flt SigmammZ) {
//https://github.com/afni/afni/blob/699775eba3c58c816d13947b81cf3a800cec606f/src/edt_blur.c
	if ((nim->nvox < 1) || (nim->nx < 2) || (nim->ny < 2) || (nim->nz < 1)) return 1;
	if (nim->datatype != DT_CALC) return 1;
	flt * img = (flt *) nim->data; 
	//int nVol = 1;
	//for (int i = 4; i < 8; i++ )
	//	nVol *= MAX(nim->dim[i],1);
	int nvox3D = nim->nx * nim->ny * MAX(nim->nz, 1);
	int nVol = nim->nvox / nvox3D;
	if ((nvox3D * nVol) != nim->nvox) return 1;
	int nx = nim->nx;
	int ny = nim->ny;
	int nz = nim->nz;	
	if (SigmammX <= 0.0) goto DO_Y_BLUR ;
	//BLUR X
	int nRow = 1;
	for (int i = 2; i < 8; i++ )
		nRow *= MAX(nim->dim[i],1);
	#if defined(_OPENMP)
	//niimath_print(">>>%d\n", omp_get_num_threads());
	if (omp_get_max_threads() > 1)
		blurP(img, nim->nx, nRow, nim->dx, SigmammX);
	else
		blurS(img, nim->nx, nRow, nim->dx, SigmammX);	
	#else
	blurS(img, nim->nx, nRow, nim->dx, SigmammX);
	#endif
	//blurX(img, nim->nx, nRow, nim->dx, SigmammX);
	DO_Y_BLUR:
	//BLUR Y
	if (SigmammY <= 0.0) goto DO_Z_BLUR ;
	nRow = nim->nx * nim->nz; //transpose XYZ to YXZ and blur Y columns with XZ Rows
	#pragma omp parallel for
	for (int v = 0; v < nVol; v++ ) { //transpose each volume separately
		flt * img3D = (flt *)_mm_malloc(nvox3D*sizeof(flt), 64); //alloc for each volume to allow openmp
		size_t vo = v * nvox3D; //volume offset
		for (int z = 0; z < nz; z++ ) {
			int zo = z * nx * ny;
			for (int y = 0; y < ny; y++ ) {
				int xo = 0;
				for (int x = 0; x < nx; x++ ) {
					img3D[zo+xo+y] = img[vo];
					vo += 1;
					xo += ny;
				}
			}
		}
		blurS(img3D, nim->ny, nRow, nim->dy, SigmammY);
		vo = v * nvox3D; //volume offset
		for (int z = 0; z < nz; z++ ) {
			int zo = z * nx * ny;
			for (int y = 0; y < ny; y++ ) {
				int xo = 0;
				for (int x = 0; x < nx; x++ ) {
					img[vo] = img3D[zo+xo+y];
					vo += 1;
					xo += ny;
				}
			}
		}		
		_mm_free (img3D);
	} //for each volume
	DO_Z_BLUR:
	//BLUR Z:
	if ((SigmammZ <= 0.0) || (nim->nz < 2)) return 0; //all done!
	nRow = nim->nx * nim->ny; //transpose XYZ to ZXY and blur Z columns with XY Rows
	//#pragma omp parallel
	//#pragma omp for
	#pragma omp parallel for
	for (int v = 0; v < nVol; v++ ) { //transpose each volume separately
		flt * img3D = (flt *)_mm_malloc(nvox3D*sizeof(flt), 64); //alloc for each volume to allow openmp
		size_t vo = v * nvox3D; //volume offset
		for (int z = 0; z < nz; z++ ) {
			for (int y = 0; y < ny; y++ ) {
				int yo = y * nz * nx;
				int xo = 0;
				for (int x = 0; x < nx; x++ ) {
					img3D[z+xo+yo] = img[vo];
					vo += 1;
					xo += nz;
				}
			}
		}
		blurS(img3D, nz, nRow, nim->dz, SigmammZ);
		vo = v * nvox3D; //volume offset
		for (int z = 0; z < nz; z++ ) {
			for (int y = 0; y < ny; y++ ) {
				int yo = y * nz * nx;
				int xo = 0;
				for (int x = 0; x < nx; x++ ) {
					img[vo] = img3D[z+xo+yo];
					vo += 1;
					xo += nz;
				} //x
			} //y
		} //z
		_mm_free (img3D);
	} //for each volume
	return 0;
}

static int nifti_otsu(nifti_image * nim, int ignoreZeroVoxels) { //binarize image using Otsu's method
	if ((nim->nvox < 1) || (nim->nx < 2) || (nim->ny < 2) || (nim->nz < 1)) return 1;
	if (nim->datatype != DT_CALC) return 1;
	flt * inimg = (flt *) nim->data; 
	flt mn = INFINITY; //better that inimg[0] in case NaN
	flt mx = -INFINITY;
	for (int i = 0; i < nim->nvox; i++ ) {
		mn = MIN(mn, inimg[i]);
		mx = MAX(mx, inimg[i]);	
	}
	if (mn >= mx) return 0; //no variability
	#define nBins 1001 
	flt scl = (nBins-1)/(mx-mn);
	int hist[nBins];
	for (int i = 0; i < nBins; i++ )
		hist[i] = 0;
	if (ignoreZeroVoxels) {
		for (int i = 0; i < nim->nvox; i++ ) {
			if (isnan(inimg[i])) continue;
			if (inimg[i] == 0.0) continue;
			hist[(int)round((inimg[i]-mn)*scl) ]++;
		}	
	} else {
		for (int i = 0; i < nim->nvox; i++ ) {
			if (isnan(inimg[i])) continue;
			hist[(int)round((inimg[i]-mn)*scl) ]++;
		}
	}
	//https://en.wikipedia.org/wiki/Otsu%27s_method	
	size_t total = 0;
	for (int i = 0; i < nBins; i++ )
		total += hist[i];
	int top = nBins - 1;
	int level = 0;
	double sumB = 0;
	double wB = 0;
	double maximum = 0.0;
	double sum1 = 0.0;
	for (int i = 0; i < nBins; i++ )
		sum1 += (i * hist[i]);
	for (int ii = 0; ii < nBins; ii++ ) {
		 double wF = total - wB;
		if ((wB > 0) && (wF > 0)) {
			double mF = (sum1 - sumB) / wF;
			double val = wB * wF * ((sumB / wB) - mF) * ((sumB / wB) - mF);
			if ( val >= maximum ) {
				level = ii;
				maximum = val;
			}
		}
		wB = wB + hist[ii];
		sumB = sumB + (ii-1) * hist[ii];
	}
	double threshold = (level / scl)+mn;
	if (ignoreZeroVoxels) {
		for (int i = 0; i < nim->nvox; i++ ) {
			if (inimg[i] == 0.0) continue;
			inimg[i] = (inimg[i] < threshold) ? 0.0 : 1.0;
		}
	} else {
		for (int i = 0; i < nim->nvox; i++ ) 
			inimg[i] = (inimg[i] < threshold) ? 0.0 : 1.0;
	}
	//niimath_message("range %g..%g threshold %g bin %d\n", mn, mx, threshold, level);
	return 0;
}

static int nifti_unsharp(nifti_image * nim, flt SigmammX, flt SigmammY, flt SigmammZ, flt amount) {
//https://github.com/afni/afni/blob/699775eba3c58c816d13947b81cf3a800cec606f/src/edt_blur.c
	if ((nim->nvox < 1) || (nim->nx < 2) || (nim->ny < 2) || (nim->nz < 1)) return 1;
	if (nim->datatype != DT_CALC) return 1;
	if (amount == 0.0) return 0;
	flt * inimg = (flt *) nim->data; 
	void * indat = (void *) nim->data; 
	flt mn = INFINITY; //better that inimg[0] in case NaN
	flt mx = -INFINITY;
	for (int i = 0; i < nim->nvox; i++ ) {
		mn = MIN(mn, inimg[i]);
		mx = MAX(mx, inimg[i]);	
	}
	if (mn >= mx) return 0; //no variability
	size_t nvox3D = nim->nx * nim->ny * MAX(nim->nz, 1);
	size_t nVol = nim->nvox / nvox3D;
	if ((nvox3D * nVol) != nim->nvox) return 1;
	//process each 3D volume independently: reduce memory pressure
	nim->nvox = nvox3D;
	void * sdat = (void *)calloc(1,nim->nvox * sizeof(flt)) ;
	nim->data = sdat;
	flt * simg = (flt *) sdat;
	for (int v = 0; v < nVol; v++ ) {
		memcpy(simg, inimg, nim->nvox*sizeof(flt)); 
		nifti_smooth_gauss(nim, SigmammX, SigmammY, SigmammZ);
		for (int i = 0; i < nim->nvox; i++ ) {
			//sharpened = original + (original - blurred) * amount
			inimg[i] += (inimg[i] - simg[i]) * amount;
			//keep in original range
			inimg[i] = MAX(inimg[i], mn);
			inimg[i] = MIN(inimg[i], mx);	
		}
		inimg += nim->nvox;
	}
	free(sdat);
	//return original data
	nim->data = indat;
	nim->nvox = nvox3D * nVol;
	return 0;
} //nifti_unsharp()

static int nifti_crop(nifti_image * nim, int tmin, int tsize) {
	if (tsize == 0) {
		niimath_message("tsize must not be 0\n");
		return 1;
	}
	if (nim->nvox < 1) return 1;
	if (nim->datatype != DT_CALC) return 1;
	int nvox3D = nim->nx * nim->ny * MAX(nim->nz, 1);
	if ((nvox3D < 1) || ((nim->nvox % nvox3D) != 0) ) return 1;
	int nvol = (nim->nvox / nvox3D); //in
	if (nvol < 2) {
		niimath_message("crop only appropriate for 4D volumes");
		return 1;
	}
	if (tmin >= nvol) {
		niimath_message("tmin must be from 0..%d, not %d\n", nvol-1, tmin);
		return 1;
	}
	int tminVol = MAX(0,tmin);
	int tFinalVol = tminVol+tsize-1; //e.g. if tmin=0 and tsize=1, tFinal=0 
	if (tsize < 0) {
		tFinalVol = INT_MAX;	
	}
	tFinalVol = MIN(tFinalVol, nvol-1);
	if ((tminVol == 0) && (tFinalVol == (nvol-1)) ) return 0;
	int nvolOut = tFinalVol-tminVol+1;
	flt * imgIn = (flt *) nim->data; 
	nim->nvox = nvox3D * nvolOut;
	void * dat = (void *)calloc(1,nim->nvox * sizeof(flt)) ;
	flt * imgOut = (flt *) dat; 
	imgIn += tminVol * nvox3D;
	memcpy(imgOut, imgIn, nim->nvox*sizeof(flt)); 
	free(nim->data);
	nim->data = dat;
	if (nvolOut == 1)
		nim->dim[0] = 3;
	else
		nim->dim[0] = 4;
	nim->ndim = nim->dim[0];
	nim->dim[4] = nvolOut;
	nim->nt = nvolOut;
	nim->nu = 1;
	nim->nv = 1;
	nim->nw = 1;
	for (int i = 5; i < 8; i++ )
		nim->dim[i] = 1;
	return 0;
}

static int nifti_rescale ( nifti_image * nim, double scale , double intercept) {
//linear transform of data
	if (nim->nvox < 1) return 1;
	if (nim->datatype == DT_CALC) {
		flt scl = scale;
		flt inter = intercept;
		flt * f32 = (flt *) nim->data;
		for (size_t i = 0; i < nim->nvox; i++ )
			f32[i] = (f32[i] * scl) + inter;
		return 0;
	}
	niimath_message("nifti_rescale: Unsupported datatype %d\n", nim->datatype);
	return 1;
}

static int nifti_tfceS(nifti_image * nim, double H, double E, int c, int x, int y, int z, double tfce_thresh) {
	if (nim->nvox < 1) return 1;
	if (nim->datatype != DT_CALC) return 1;
	if ((x < 0) || (x >= nim->dim[1]) || (y < 0) || (y >= nim->dim[2]) || (z < 0) || (z >= nim->dim[3])) {
		niimath_message("tfceS x/y/z must be in range 0..%"PRId64"/0..%"PRId64"/0..%"PRId64"\n", nim->dim[1]-1, nim->dim[2]-1, nim->dim[3]-1);
	}
	if (!neg_determ(nim)) 
		x = nim->dim[1] - x - 1;
	int seed = x + (y * nim->dim[1]) + (z * nim->dim[1] * nim->dim[2]);
	flt * inimg = (flt *) nim->data;
	if (inimg[seed] < H) {
		niimath_message("it doesn't reach to specified threshold\n");
		return 1;
	}
	size_t nvox3D = nim->dim[1]*nim->dim[2]*nim->dim[3];
	if (nim->nvox > nvox3D) { 
		niimath_message("tfceS not suitable for 4D data.\n");
		return 1; 
	}
	//niimath_print("peak %g\n", inimg[seed]);
	int numk = c;
	if ((c != 6) && (c != 18) && (c != 26)) {
		niimath_message("suitable values for c are 6, 18 or 26\n");
		numk = 6;	
	}
	//set up kernel to search for neighbors. Since we already included sides, we do not worry about A<->P and L<->R wrap
	int32_t * k = (int32_t *)_mm_malloc(3*numk*sizeof(int32_t), 64); //kernel: offset, x, y
	int mxDx = 1; //connectivity 6: faces only
	if (numk == 18) mxDx = 2; //connectivity 18: faces+edges
	if (numk == 26) mxDx = 3; //connectivity 26: faces+edges+corners
	int j = 0;
	for (int z = -1; z <= 1; z++ )
		for (int y = -1; y <= 1; y++ )
			for (int x = -1; x <= 1; x++ ) {
				int dx = abs(x)+abs(y)+abs(z);
				if ((dx > mxDx) || (dx == 0)) continue; 
				k[j] = x + (y * nim->nx) + (z * nim->nx * nim->ny);
				k[j+numk] = x; //avoid left-right wrap
				k[j+numk+numk] = x; //avoid anterior-posterior wrap				
				j++;
			} //for x
	flt mx = (inimg[0]);
	for (size_t i = 0; i < nvox3D; i++ )
		mx = MAX((inimg[i]),mx);	
	double dh = mx/100.0;
	
	flt * outimg = (flt *)_mm_malloc(nvox3D*sizeof(flt), 64); //output image
	int32_t * q = (int32_t *)_mm_malloc(nvox3D*sizeof(int32_t), 64); //queue with untested seed
	uint8_t * vxs = (uint8_t *)_mm_malloc(nvox3D*sizeof(uint8_t), 64); 
	for (int i = 0; i < nvox3D; i++ )
		outimg[i] = 0.0;	
	int n_steps = (int)ceil(mx/dh);
	//for (int step=0; step<n_steps; step++) {
	for (int step=n_steps-1; step >= 0; step--) {
		flt thresh = (step+1)*dh;
		memset(vxs, 0, nvox3D*sizeof(uint8_t));
		for (int i = 0; i < nvox3D; i++ )
			if (inimg[i] >= thresh)
				vxs[i] = 1; //survives, unclustered
		int qlo = 0;
		int qhi = 0; 
		q[qhi] = seed; //add starting voxel as seed in queue
		vxs[seed] = 0; //do not find again!
		while (qhi >= qlo) { //first in, first out queue
			//retire one seed, add 0..6, 0..18 or 0..26 new ones (depending on connectivity)
			for (int j = 0; j < numk; j++) {
				int jj = q[qlo] + k[j];
				if ((jj < 0) || (jj >= nvox3D)) continue; //voxel in volume
				if (vxs[jj] == 0) continue; //already found or did not survive threshold
				int dx = x+k[j+numk];
				if ((dx < 0) || (dx >= nim->nx)) continue; //wrapped left-right
				int dy = y+k[j+numk+numk];
				if ((dy < 0) || (dy >= nim->ny)) continue; //wrapped anterior-posterior
				//add new seed:
				vxs[jj] = 0; //do not find again!
				qhi++;
				q[qhi] = jj;	 
			}        	
			qlo++;
		} //while qhi >= qlo: continue until all seeds tested
		flt valToAdd = pow(qhi+1, E) * pow(thresh, H); //"supporting section", Dark Gray in Figure 1
		for (int j = 0; j <= qhi; j++)
			outimg[q[j]] += valToAdd;
		//niimath_print("step %d thresh %g\n", step, outimg[seed]);
		if (outimg[seed] >= tfce_thresh)
			break;
	} //for each step
	if ( outimg[seed] < tfce_thresh) 
		niimath_message("it doesn't reach to specified threshold (%g < %g)\n", outimg[seed], tfce_thresh);
	for (size_t i = 0; i < nvox3D; i++ )
		if (outimg[i] == 0.0)
			inimg[i] =	0.0;
	_mm_free (q);
	_mm_free (vxs);
	_mm_free (outimg);
	_mm_free (k);
	return 0;
}

static int nifti_tfce(nifti_image * nim, double H, double E, int c)  { 
	//https://www.fmrib.ox.ac.uk/datasets/techrep/tr08ss1/tr08ss1.pdf
	if (nim->nvox < 1) return 1;
	if (nim->datatype != DT_CALC) return 1;
	int nvox3D = nim->dim[1] * nim->dim[2] * nim->dim[3];
	int nvol = nim->nvox / nvox3D;
	int numk = c;
	if ((c != 6) && (c != 18) && (c != 26)) {
		niimath_message("suitable values for c are 6, 18 or 26\n");
		numk = 6;	
	}
	//set up kernel to search for neighbors. Since we already included sides, we do not worry about A<->P and L<->R wrap
	int32_t * k = (int32_t *)_mm_malloc(3*numk*sizeof(int32_t), 64); //kernel: offset, x, y
	int mxDx = 1; //connectivity 6: faces only
	if (numk == 18) mxDx = 2; //connectivity 18: faces+edges
	if (numk == 26) mxDx = 3; //connectivity 26: faces+edges+corners
	int j = 0;
	for (int z = -1; z <= 1; z++ )
		for (int y = -1; y <= 1; y++ )
			for (int x = -1; x <= 1; x++ ) {
				int dx = abs(x)+abs(y)+abs(z);
				if ((dx > mxDx) || (dx == 0)) continue; 
				k[j] = x + (y * nim->nx) + (z * nim->nx * nim->ny);
				k[j+numk] = x; //avoid left-right wrap
				k[j+numk+numk] = x; //avoid anterior-posterior wrap				
				j++;
			} //for x
	//omp notes: here we compute each volume independently. 
	//    Christian Gaser computes the step loop in parallel, which accelerates 3D cases
	//    This code is very quick on 3D, so this does not seem crucial, and avoids critical sections
	#pragma omp parallel for
	for (int vol = 0; vol < nvol; vol++ ) {
		//identify clusters
		flt * inimg = (flt *) nim->data;
		inimg += vol * nvox3D;
		flt mx = (inimg[0]);
		for (size_t i = 0; i < nvox3D; i++ )
			mx = MAX((inimg[i]),mx);	
		double dh = mx/100.0;
		flt * outimg = (flt *)_mm_malloc(nvox3D*sizeof(flt), 64); //output image
		int32_t * q = (int32_t *)_mm_malloc(nvox3D*sizeof(int32_t), 64); //queue with untested seed
		uint8_t * vxs = (uint8_t *)_mm_malloc(nvox3D*sizeof(uint8_t), 64); 
		for (int i = 0; i < nvox3D; i++ )
			outimg[i] = 0.0;	
		int n_steps = (int)ceil(mx/dh);
		for (int step=0; step<n_steps; step++) {
			flt thresh = (step+1)*dh;
			memset(vxs, 0, nvox3D*sizeof(uint8_t));
			for (int i = 0; i < nvox3D; i++ )
				if (inimg[i] >= thresh)
					vxs[i] = 1; //survives, unclustered
			int i = 0;
			for (int z = 0; z < nim->nz; z++ )
				for (int y = 0; y < nim->ny; y++ )
					for (int x = 0; x < nim->nx; x++ ) {
						if (vxs[i] == 0) {
							i++;
							continue;
						} //voxel did not survive or already clustered
						int qlo = 0;
						int qhi = 0; 
						q[qhi] = i; //add starting voxel as seed in queue
						vxs[i] = 0; //do not find again!
						while (qhi >= qlo) { //first in, first out queue
							//retire one seed, add 0..6, 0..18 or 0..26 new ones (depending on connectivity)
							for (int j = 0; j < numk; j++) {
								int jj = q[qlo] + k[j];
								if ((jj < 0) || (jj >= nvox3D)) continue; //voxel in volume
								if (vxs[jj] == 0) continue; //already found or did not survive threshold
								int dx = x+k[j+numk];
								if ((dx < 0) || (dx >= nim->nx)) continue; //wrapped left-right
								int dy = y+k[j+numk+numk];
								if ((dy < 0) || (dy >= nim->ny)) continue; //wrapped anterior-posterior
								//add new seed:
								vxs[jj] = 0; //do not find again!
								qhi++;
								q[qhi] = jj;	 
							}        	
							qlo++;
						} //while qhi >= qlo: continue until all seeds tested
						flt valToAdd = pow(qhi+1, E) * pow(thresh, H); //"supporting section", Dark Gray in Figure 1
						for (int j = 0; j <= qhi; j++)
							outimg[q[j]] += valToAdd;
						i++;
					} //for each voxel
		} //for each step
		for (int i = 0; i < nvox3D; i++ )
			inimg[i] = outimg[i];
		_mm_free (q);
		_mm_free (vxs);
		_mm_free (outimg);
	}
	_mm_free (k);	
	return 0;
} //nifti_tfce()
		
static int nifti_grid( nifti_image * nim, double v, int spacing) {
	if ((nim->nvox < 1) || (nim->nx < 2) || (nim->ny < 2)) return 1;
	if (nim->datatype != DT_CALC) return 1;
	size_t nxy = (nim->nx * nim->ny);
	size_t nzt = nim->nvox / nxy;
	flt * f32 = (flt *) nim->data;
	flt fv = v;
	#pragma omp parallel for
	for (size_t i = 0; i < nzt; i++ ) { //for each 2D slices
		size_t so = i * nxy; //slice offset
		int z = (i % nim->nz);
		if ((nim->nz > 1) && ((z % spacing) == 0) ) { //whole slice is grid
			for (size_t j = 0; j < nxy; j++ )
				f32[so++] = fv;
			continue;
		}
		for (size_t y = 0; y < nim->ny; y++ )
			for (size_t x = 0; x < nim->nx; x++ ) {
				if ((x % spacing) == 0) f32[so] = fv;
				so ++;
			}
		so = i * nxy; //slice offset
		for (size_t y = 0; y < nim->ny; y++ )
			for(size_t x = 0; x < nim->nx; x++ ) {
				if ((y % spacing) == 0) f32[so] = fv;
				so ++;
			}	
	} //for i: each 2D slice
	return 0;
}

static int nifti_rem ( nifti_image * nim, double v, int isFrac) {
//remainder (modulo) : fslmaths
/*fmod(0.45, 2) = 0.45 : 0
fmod(0.9, 2) = 0.9 : 0
fmod(1.35, 2) = 1.35 : 1
fmod(1.8, 2) = 1.8 : 1
fmod(-0.45, 2) = -0.45 : 0
fmod(-0.9, 2) = -0.9 : 0
fmod(-1.35, 2) = -1.35 : -1
fmod(-1.8, 2) = -1.8 : -1
*/
	if (nim->datatype != DT_CALC) return 1;
	if (nim->nvox < 1) return 1;
	if (v == 0.0) {
		niimath_message("Exception: '-rem 0' does not make sense\n");
		return 1;	
	}
	flt fv = v;
	flt * f32 = (flt *) nim->data;
	if (isFrac) {
		for (size_t i = 0; i < nim->nvox; i++ ) 
		f32[i] = fmod(f32[i], fv);
	} else {
		for (size_t i = 0; i < nim->nvox; i++ ) {
			//niimath_print("fmod(%g, %g) = %g : %g\n", f32[i], fv, fmod(f32[i],fv), trunc(fmod(f32[i],fv)) );  
			f32[i] = trunc(fmod(f32[i], fv));
		}
	}
	return 0;
}

static int nifti_thr( nifti_image * nim, double v, int zeroBrightVoxels) {
	if (nim->nvox < 1) return 1;
	if (nim->datatype == DT_CALC) {
		flt fv = v;
		flt * f32 = (flt *) nim->data;
		if (zeroBrightVoxels) {
			for (size_t i = 0; i < nim->nvox; i++ )
				if (f32[i] > fv)
					f32[i] = 0.0f;
		
		} else {
			for (size_t i = 0; i < nim->nvox; i++ )
				if (f32[i] < fv)
					f32[i] = 0.0f;
		}
		return 0;
	}
	niimath_message("nifti_thr: Unsupported datatype %d\n", nim->datatype);
	return 1;
} // nifti_thr()

static int nifti_max( nifti_image * nim, double v, int useMin) {
	if (nim->nvox < 1) return 1;
	if (nim->datatype == DT_CALC) {
		flt fv = v;
		flt * f32 = (flt *) nim->data;
		if (useMin) {
			for (size_t i = 0; i < nim->nvox; i++ )
				f32[i] = fmin(f32[i], fv);
		
		} else {
			for (size_t i = 0; i < nim->nvox; i++ )
				f32[i] = fmax(f32[i], fv);
		}
		return 0;
	}
	niimath_message("nifti_max: Unsupported datatype %d\n", nim->datatype);
	return 1;
} // nifti_max()

static int nifti_inm( nifti_image * nim, double M) {
//https://www.jiscmail.ac.uk/cgi-bin/webadmin?A2=fsl;bf9d21d2.1610
//With '-inm <value>', every voxel in the input volume is multiplied by <value> / M
// where M is the mean across all voxels.
//n.b.: regardless of description, mean appears to only include voxels > 0
	if (nim->nvox < 1) return 1;
	if (nim->datatype != DT_CALC) return 1;
	int nvox3D = nim->nx * nim->ny * MAX(nim->nz, 1);
	if ((nvox3D < 1) || ((nim->nvox % nvox3D) != 0) ) return 1;
	int nvol = nim->nvox / nvox3D;
	flt * f32 = (flt *) nim->data;
	#pragma omp parallel for
	for (int v = 0; v < nvol; v++ ) {
		size_t vi = v * nvox3D;
		double sum = 0.0;
		#define gt0	
		#ifdef gt0
		int n = 0;
		for (size_t i = 0; i < nvox3D; i++ ) {
			if (f32[vi+i] > 0.0f) {
				n ++;
				sum += f32[vi+i];
			}
		}
		if (sum == 0.0) continue;
		double ave = sum / n;
		#else
		for (int i = 0; i < nvox3D; i++ )
			sum += f32[vi+i];
		if (sum == 0.0) continue;
		double ave = sum / nvox3D;
		#endif
		//niimath_print("%g %g\n", ave, M);
		flt scale = M / ave;
		for (int i = 0; i < nvox3D; i++ )
			f32[vi+i] *= scale;
	}
	return 0;
} // nifti_inm()

static int nifti_ing( nifti_image * nim, double M) {
//https://www.jiscmail.ac.uk/cgi-bin/webadmin?A2=fsl;bf9d21d2.1610
//With '-inm <value>', every voxel in the input volume is multiplied by <value> / M
// where M is the mean across all voxels.
//n.b.: regardless of description, mean appears to only include voxels > 0
	if (nim->nvox < 1) return 1;
	if (nim->datatype != DT_CALC) return 1;
	flt * f32 = (flt *) nim->data;
	double sum = 0.0;
	int n = 0;
	for (size_t i = 0; i < nim->nvox; i++ ) {
		if (f32[i] > 0.0f) {
			n ++;
			sum += f32[i];
		}	
	}
	if (sum == 0) return 0;	
	double ave = sum / n;
	flt scale = M / ave;
	#pragma omp parallel for
	for (int i = 0; i < nim->nvox; i++ )
		f32[i] *= scale;
	return 0;
} //nifti_ing()

static int nifti_robust_range(nifti_image * nim, flt * pct2, flt * pct98, int ignoreZeroVoxels) {
//https://www.jiscmail.ac.uk/cgi-bin/webadmin?A2=fsl;31f309c1.1307
// robust range is essentially the 2nd and 98th percentiles
// "but ensuring that the majority of the intensity range is captured, even for binary images." 
// fsl uses 1000 bins, also limits for volumes less than 100 voxels taylor.hanayik@ndcn.ox.ac.uk 20190107
//fslstats trick -r
// 0.000000 1129.141968
//niimath >fslstats trick -R
// 0.000000 2734.000000 
	*pct2 = 0.0;
	*pct98 = 1.0;
	if (nim->nvox < 1) return 1;
	if (nim->datatype != DT_CALC) return 1;
	flt * f32 = (flt *) nim->data;
	flt mn = INFINITY;
	flt mx = -INFINITY;
	size_t nZero = 0;
	size_t nNan = 0;
	for (size_t i = 0; i < nim->nvox; i++ ) {
		if (isnan(f32[i])) {
			nNan ++;
			continue;
		}
		if ( f32[i] == 0.0 ) {
			nZero++;
			continue;
		}
		mn = fmin(f32[i],mn);
		mx = fmax(f32[i],mx);

	}
	if ((nZero > 0) && (mn > 0.0) && (!ignoreZeroVoxels)) 
		mn = 0.0;
	if (mn > mx) return 0; //all NaN
	if (mn == mx) {
		*pct2 = mn;
		*pct98 = mx;
	
		return 0;
	}
	if (!ignoreZeroVoxels)
		nZero = 0;
	nZero += nNan;
	size_t n2pct = round((nim->nvox - nZero)* 0.02);
	if ((n2pct < 1) || (mn == mx) || ((nim->nvox -nZero) < 100) ) { //T Hanayik mentioned issue with very small volumes 
		*pct2 = mn;
		*pct98 = mx;
		return 0;
	}
	#define nBins 1001 
	flt scl = (nBins-1)/(mx-mn);
	int hist[nBins];
	for (int i = 0; i < nBins; i++ )
		hist[i] = 0;
	if (ignoreZeroVoxels) {
		for (int i = 0; i < nim->nvox; i++ ) {
			if (isnan(f32[i])) continue;
			if (f32[i] == 0.0) continue;
			hist[(int)round((f32[i]-mn)*scl) ]++;
		}	
	} else {
		for (int i = 0; i < nim->nvox; i++ ) {
			if (isnan(f32[i])) continue;
			hist[(int)round((f32[i]-mn)*scl) ]++;
		}
	}	
	size_t n = 0;
	size_t lo = 0;
	while (n < n2pct) {
		n += hist[lo];
		//if (lo < 10)
		//	niimath_print("%zu %zu %zu %d\n",lo, n, n2pct, ignoreZeroVoxels);
		lo++;
	}
	lo --; //remove final increment
	n = 0;
	int hi = nBins;
	while (n < n2pct) {
		hi--;
		n += hist[hi];
	}
	/*if ((lo+1) < hi) {
		size_t nGray = 0;
		for (int i = lo+1; i < hi; i++ ) {
			nGray += hist[i];
			//niimath_print("%d %d\n", i, hist[i]);
		}
		float fracGray = (float)nGray/(float)(nim->nvox - nZero);
		niimath_print("histogram[%d..%d] = %zu %g\n", lo, hi, nGray, fracGray);
	}*/
	if (lo == hi) { //MAJORITY are not black or white
		int ok = -1;
		while (ok != 0) {
			if (lo > 0) {
				lo--;
				if (hist[lo] > 0) ok = 0;	
			}
			if ((ok != 0) && (hi < (nBins-1))) {
				hi++;
				if (hist[hi] > 0) ok = 0;	
			}
			if ((lo == 0) && (hi == (nBins-1))) ok = 0;
		} //while not ok
	} //if lo == hi
	*pct2 = (lo)/scl + mn; 
	*pct98 = (hi)/scl + mn;
	niimath_print("full range %g..%g (voxels 0 or NaN =%zu)  robust range %g..%g\n", mn, mx, nZero, *pct2, *pct98);
	return 0;
}

enum eDimReduceOp{Tmean,Tstd,Tmax,Tmaxn,Tmin,Tmedian,Tperc,Tar1};

static int compare (const void * a, const void * b) {
	flt fa = *(const flt*) a;
	flt fb = *(const flt*) b;
	return (fa > fb) - (fa < fb);
}

static void dtrend(flt * xx, int npt, int pt0) {
	//linear detrend, first point is set to zero
	// if pt0=0 then mean is zero, pt0=1 then first point is zero, if pt0=2 final point is zero
	double t1,t3,t10 , x0,x1 ;
	int ii ;
	if( npt < 2 || xx == NULL ) return ;
	x0 = xx[0] ; x1 = 0.0 ;
	for( ii=1 ; ii < npt ; ii++ ){
		x0 += xx[ii] ;
		x1 += xx[ii] * ii ;
	}
	t1 = npt*x0; t3 = 1.0/npt; t10 = npt*npt;
	double f0 = (double)(2.0/(npt+1.0)*t3*(2.0*t1-3.0*x1-x0));
	double f1 = (double)(-6.0/(t10-1.0)*t3*(-x0-2.0*x1+t1));
	//niimath_print("%.8g %.8g %g\n", f0, f1, xx[0]); 
	if (pt0 == 1) f0 = xx[0];
	if (pt0 == 2) f0 = xx[npt-1]- (f1*(npt-1));
	for( ii=0 ; ii < npt ; ii++ ) xx[ii] -= (f0 + f1*ii) ;
}

static int nifti_detrend_linear(nifti_image * nim) {
	if (nim->datatype != DT_CALC) return 1;
	size_t nvox3D = nim->nx * nim->ny * MAX(1,nim->nz);
	if (nvox3D < 1) return 1; 
	int nvol = nim->nvox / nvox3D;
	if ((nvox3D * nvol) != nim->nvox) return 1;
	if (nvol < 2) {
		niimath_message("detrend requires a 4D image with at least three volumes\n");
		return 1;
	}
	flt * img = (flt *) nim->data;
	#pragma omp parallel for
	for (size_t i = 0; i < nvox3D; i++) {
		flt * data = (flt *)_mm_malloc(nvol*sizeof(flt), 64);
    	//load one voxel across all timepoints
    	int j = 0;
    	for (size_t v = i; v < nim->nvox; v+= nvox3D) {
			data[j] = img[v];
			j++;
		}
		//detrend
		dtrend(data, nvol, 0);
		//save one voxel across all timepoints
    	j = 0;
		for (size_t v = i; v < nim->nvox; v+= nvox3D) {
			img[v] = data[j];
			j++;
		}
    	_mm_free (data);
	}
	return 0;
}

#ifdef bandpass
//https://github.com/QtSignalProcessing/QtSignalProcessing/blob/master/src/iir.cpp
//https://github.com/rkuchumov/day_plot_diagrams/blob/8df48af431dc76b1656a627f1965d83e8693ddd7/data.c
//https://scipy-cookbook.readthedocs.io/items/ButterworthBandpass.html
// Sample rate and desired cutoff frequencies (in Hz).
//  double highcut = 1250;
//  double lowcut = 500;
//  double samp_rate = 5000;
//[b,a] = butter(2, [0.009, 0.08]);
//https://afni.nimh.nih.gov/afni/community/board/read.php?1,84373,137180#msg-137180
//Power 2011, Satterthwaite 2013, Carp 2011, Power's reply to Carp 2012
// https://github.com/lindenmp/rs-fMRI/blob/master/func/ButterFilt.m
//https://docs.scipy.org/doc/scipy/reference/generated/scipy.signal.filtfilt.html

/*
The function butterworth_filter() emulates Jan Simon's FiltFiltM
 it uses Gustafsson’s method and padding to reduce ringing at start/end
https://www.mathworks.com/matlabcentral/fileexchange/32261-filterm?focused=5193423&tab=function
Copyright (c) 2011, Jan Simon
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are
met:

    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in
      the documentation and/or other materials provided with the distribution

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.*/
static int butterworth_filter(flt * img, int nvox3D, int nvol, double fs, double highcut, double lowcut) {
//sample rate, low cut and high cut are all in Hz
//this attempts to emulate performance of https://www.mathworks.com/matlabcentral/fileexchange/32261-filterm
//  specifically, prior to the forward and reverse pass the coefficients are estimated by a forward and reverse pass
	int order = 2;
	if (order <= 0) return 1;
    if ((highcut <= 0.0) && (lowcut <= 0.0)) return 1;
    if (fs <= 0.0) return 1;
	if ((lowcut > 0.0) && (highcut > 0.0))
		niimath_print("butter bandpass lowcut=%g highcut=%g fs=%g order=%d (effectively %d due to filtfilt)\n", lowcut, highcut, fs, order, 2*order); 	
	else if (highcut > 0.0)
		niimath_print("butter lowpass highcut=%g fs=%g order=%d (effectively %d due to filtfilt)\n", highcut, fs, order, 2*order); 	
	else if (lowcut > 0.0)
		niimath_print("butter highpass lowcut=%g fs=%g order=%d (effectively %d due to filtfilt)\n", lowcut, fs, order, 2*order);
	else {
		niimath_print("Butterworth parameters do not make sense\n");
		return 1;
	}    
    double * a;
	double * b;
	double * IC;
	int nX = nvol;
	int nA = 0;
	nA = butter_design(order, 2.0*lowcut/fs, 2.0*highcut/fs, &a, &b, &IC);
	int nEdge = 3 * (nA -1);
	if ((nA < 1) || (nX <= nEdge)) {
		niimath_print("filter requires at least %d samples\n", nEdge);
		_mm_free(a);
		_mm_free(b);
		_mm_free(IC);
		return 1;
	}
	#pragma omp parallel for	
    for (int vx = 0; vx < nvox3D; vx++) {
    	double * X = (double *)_mm_malloc(nX*sizeof(double), 64);
    	size_t vo = vx;
    	flt mn = INFINITY;
    	flt mx = -INFINITY;
    	for (int j = 0; j < nX; j++) {
			X[j] = img[vo];
			mn = MIN(mn, X[j]);
			mx = MAX(mx, X[j]);
			vo += nvox3D;	
		}
		if (mn < mx) { //some variability
			double * Xi = (double *)_mm_malloc(nEdge * sizeof(double), 64);
			for (int i = 0; i < nEdge; i++)
				Xi[nEdge-i-1] = X[0]-(X[i+1]-X[0]);
			double * CC = (double *)_mm_malloc((nA-1) * sizeof(double), 64);
			for (int i = 0; i < (nA-1); i++)
				CC[i] = IC[i]* Xi[0];
			double * Xf = (double *)_mm_malloc(nEdge * sizeof(double), 64);
			for (int i = 0; i < nEdge; i++)
				Xf[i] = X[nX-1]-(X[nX-2-i]-X[nX-1]);
			Filt(Xi, nEdge, a, b, nA-1, CC); //filter head
			Filt(X, nX, a, b, nA-1, CC); //filter array
			Filt(Xf, nEdge, a, b, nA-1, CC); //filter tail
			//reverse
			for (int i = 0; i < (nA-1); i++)
				CC[i] = IC[i]* Xf[nEdge-1];
			FiltRev(Xf, nEdge, a, b, nA-1, CC); //filter tail
			FiltRev(X, nX, a, b, nA-1, CC); //filter array
			_mm_free (Xi);
    		_mm_free (Xf);
    		_mm_free (CC);
		} else { //else no variability: set all voxels to zero
			for (int j = 0; j < nX; j++)
				 X[j] = 0;
		}
		//save data to 4D array
		vo = vx;
    	for (int j = 0; j < nX; j++) {
			img[vo] = X[j];
			vo += nvox3D;	
		}
    	_mm_free (X);
    } //for vx
    _mm_free(b);
    _mm_free(a);
    _mm_free(IC);
    return 0;
}

static int nifti_bandpass(nifti_image * nim, double hp_hz, double lp_hz, double TRsec) { 
	if (nim->datatype != DT_CALC) return 1;
	size_t nvox3D = nim->nx * nim->ny * MAX(1,nim->nz);
	if (TRsec <= 0.0) 
		TRsec = nim->pixdim[4];
	if (TRsec <= 0) {
		niimath_message("Unable to determine sample rate\n");
		return 1;
	}	
	if (nvox3D < 1) return 1; 
	int nvol = nim->nvox / nvox3D;
	if ((nvox3D * nvol) != nim->nvox) return 1;
	if (nvol < 1) {
		niimath_message("bandpass requires 4D datasets\n");
		return 1;
	}
	return butterworth_filter((flt *) nim->data, nvox3D, nvol, 1/TRsec, hp_hz, lp_hz);
}
#endif

static int nifti_bptf(nifti_image * nim, double hp_sigma, double lp_sigma, int demean) { 
//Spielberg Matlab code: https://cpb-us-w2.wpmucdn.com/sites.udel.edu/dist/7/4542/files/2016/09/fsl_temporal_filt-15sywxn.m
//5.0.7 highpass temporal filter removes the mean component https://fsl.fmrib.ox.ac.uk/fsl/fslwiki/WhatsNew#anchor1
/*
http://www.fast.u-psud.fr/ezyfit/html/ezfit.html
fitting functions are: 
   - linear             y = m * x 
   - affine or poly1    y = a*x + b 
   - poly{n}            y = a0 + a1 * x + ... + an * x^n 
   - power              y = c*x^n 
   - sin                y = a * sin (b * x) 
   - cos                y = a * cos (b * x) 
   - exp                y = a * exp (b * x) 
   - log                y = a * log (b * x) 
   - cngauss            y = exp(-x^2/(2*s^2))/(2*pi*s^2)^(1/2) 
   - cfgauss            y = a*exp(-x^2/(2*s^2)) 
   - ngauss             y = exp(-(x-x0)^2/(2*s^2))/(2*pi*s^2)^(1/2) 
   - gauss              y = a*exp(-(x-x0)^2/(2*s^2))
*/
    // y = a*exp(-(x-x0)^2/(2*s^2))
    // regression formula (https://www.mathsisfun.com/data/least-squares-regression.html) modulated by weight
	if (nim->datatype != DT_CALC) return 1;
	if ((hp_sigma <= 0) && (lp_sigma <= 0)) return 0;
	size_t nvox3D = nim->nx * nim->ny * MAX(1,nim->nz);
	if (nvox3D < 1) return 1; 
	int nvol = nim->nvox / nvox3D;
	if ((nvox3D * nvol) != nim->nvox) return 1;
	if (nvol < 1) {
		niimath_message("bptf requires 4D datasets\n");
		return 1;
	}
	int * hpStart, * hpEnd;
	double * hpSumX, * hpDenom, * hpSumWt, * hp, * hp0;
	if (hp_sigma > 0) { //initialize high-pass reusables
		//Spielberg's code uses 8*sigma, does not match current fslmaths:
		//tested with fslmaths freq4d -bptf 10 -1 nhp
		//cutoff ~3: most difference: 4->0.0128902 3->2.98023e-08 2->-0.0455322 1->0.379412
		int cutoffhp = ceil(3*hp_sigma); //to do: check this! ~3
		hp = (double *)_mm_malloc((cutoffhp+1+cutoffhp)*sizeof(double), 64); //-cutoffhp..+cutoffhp
		hp0 = hp + cutoffhp; //convert from 0..(2*cutoffhp) to -cutoffhp..+cutoffhp
		for (int k = -cutoffhp; k <= cutoffhp; k++)  //for each index in kernel
			hp0[k] = exp(-sqr(k)/(2 * sqr(hp_sigma)));
		hpStart = (int *)_mm_malloc(nvol*sizeof(int), 64); 
		hpEnd = (int *)_mm_malloc(nvol*sizeof(int), 64); 
		hpSumX = (double *)_mm_malloc(nvol*sizeof(double), 64); //
		hpDenom = (double *)_mm_malloc(nvol*sizeof(double), 64); // N*Sum(x^2) - (Sum(x))^2
		hpSumWt = (double *)_mm_malloc(nvol*sizeof(double), 64); //sum of weight, N
		for (int v = 0; v < nvol; v++) {
			//linear regression with "gauss" fitting
			hpStart[v] = MAX(0,v-cutoffhp);
			hpEnd[v] = MIN(nvol-1,v+cutoffhp);
			double sumX = 0.0;
			double sumX2 = 0.0;
			double sumWt = 0.0;
			for (int k = hpStart[v]; k <= hpEnd[v]; k++) { //for each index in kernel
				int x = k-v;
				double wt = hp0[x]; //kernel weight
				sumX += wt * x;
				sumX2 += wt * x * x;
				sumWt += wt;	
			}
			hpSumX[v] = sumX;
			hpDenom[v] = (sumWt * sumX2) - sqr(sumX);  // N*Sum(x^2) - (Sum(x))^2
			if (hpDenom[v] == 0.0) hpDenom[v] = 1.0; //should never happen, x is known index
			hpDenom[v] = 1.0 / hpDenom[v]; //use reciprocal so we can use faster multiplication later
			hpSumWt[v] = sumWt;	
		} //for each volume
	} //high-pass reusables
	//low-pass AFTER high-pass: fslmaths freq4d -bptf 45 5  fbp
	int * lpStart, * lpEnd;
	double * lpSumWt, * lp, * lp0;
	if (lp_sigma > 0) { //initialize low-pass reusables
		//simple Gaussian blur in time domain
		//freq4d -bptf -1 5  flp 
		// fslmaths rest -bptf -1 5  flp
		// 3->0.00154053 4->3.5204e-05 5->2.98023e-07, 6->identical
		// Spielberg's code uses 8*sigma, so we will use that, even though precision seems excessive
		int cutofflp = ceil(8*lp_sigma); //to do: check this! at least 6
		lp = (double *)_mm_malloc((cutofflp+1+cutofflp)*sizeof(double), 64); //-cutofflp..+cutofflp
		lp0 = lp + cutofflp; //convert from 0..(2*cutofflp) to -cutofflp..+cutofflp
		for (int k = -cutofflp; k <= cutofflp; k++) //for each index in kernel
			lp0[k] = exp(-sqr(k)/(2 * sqr(lp_sigma)));
		lpStart = (int *)_mm_malloc(nvol*sizeof(int), 64); 
		lpEnd = (int *)_mm_malloc(nvol*sizeof(int), 64); 
		lpSumWt = (double *)_mm_malloc(nvol*sizeof(double), 64); //sum of weight, N
		for (int v = 0; v < nvol; v++) {
			lpStart[v] = MAX(0,v-cutofflp);
			lpEnd[v] = MIN(nvol-1,v+cutofflp);
			double sumWt = 0.0;
			for (int k = lpStart[v]; k <= lpEnd[v]; k++) //for each index in kernel
				sumWt += lp0[k-v]; //kernel weight
			if (sumWt == 0.0) sumWt = 1.0; //will never happen
			lpSumWt[v] = 1.0 / sumWt; //use reciprocal so we can use faster multiplication later	
		} //for each volume
	} //low-pass reusables
	//https://www.jiscmail.ac.uk/cgi-bin/webadmin?A2=FSL;5b8cace9.0902
	//if  TR=2s and 100 second cutoff is requested choose "-bptf 50 -1"
	//The 'cutoff' is defined as the FWHM of the filter, so if you ask for  
	//100s that means 50 Trs, so the sigma, or HWHM, is 25 TRs.  
	// -bptf  <hp_sigma> <lp_sigma>	
	flt * img = (flt *) nim->data;
	#pragma omp parallel for
	for (size_t i = 0; i < nvox3D; i++) {
		//read input data
		flt * imgIn = (flt *)_mm_malloc((nvol)*sizeof(flt), 64); 
		flt * imgOut = (flt *)_mm_malloc((nvol)*sizeof(flt), 64); 
		int j = 0;
		for (size_t v = i; v < nim->nvox; v+= nvox3D) {
			imgIn[j] = img[v];
			j++;	
		}
		if (hp_sigma > 0) {
			double sumOut = 0.0;
			for (int v = 0; v < nvol; v++) { //each volume
				double sumY = 0.0;
				double sumXY = 0.0;
				for (int k = hpStart[v]; k <= hpEnd[v]; k++) { //for each index in kernel
					int x = k-v;
					double wt = hp0[x];
					flt y = imgIn[k];
					sumY += wt * y;
					sumXY += wt * x * y;	
				}
				double n = hpSumWt[v];
				double m = ((n*sumXY) - (hpSumX[v] * sumY) ) * hpDenom[v]; //slope
				double b = (sumY - (m * hpSumX[v]))/n; //intercept
				imgOut[v] = imgIn[v] - b;
				sumOut += imgOut[v];
			} //for each volume
			//"fslmaths -bptf removes timeseries mean (for FSL 5.0.7 onward)" n.b. except low-pass
			double mean = sumOut / (double)nvol; //de-mean AFTER high-pass
			if (demean) {
				for (int v = 0; v < nvol; v++) //each volume
					imgOut[v] -= mean;
			}	
		} //hp_sigma > 0
		if (lp_sigma > 0) { //low pass does not de-mean data
			//if BOTH low-pass and high-pass, apply low pass AFTER high pass:
			//  fslmaths freq4d -bptf 45 5  fbp
			//  difference 1.86265e-08
			//still room for improvement:
			//  fslmaths /Users/chris/src/rest -bptf 45 5  fbp
			//   r=1.0 identical voxels 73% max difference 0.000488281
			if (hp_sigma > 0) 
				memcpy(imgIn, imgOut, nvol*sizeof(flt));
			for (int v = 0; v < nvol; v++) { //each volume
				double sum = 0.0;
				for (int k = lpStart[v]; k <= lpEnd[v]; k++) //for each index in kernel
					sum += imgIn[k] * lp0[k-v];
				imgOut[v] = sum * lpSumWt[v];
			} // for each volume
		}  //lp_sigma > 0
		//write filtered data
		j = 0;
        for (size_t v = i; v < nim->nvox; v+= nvox3D) {
			img[v] = imgOut[j];
			j++;	
		}
		_mm_free (imgIn);
		_mm_free (imgOut);
	}
	if (hp_sigma > 0) { //initialize high-pass reuseables
		_mm_free (hp);
		_mm_free (hpStart);
		_mm_free (hpEnd);
		_mm_free (hpSumX);
		_mm_free (hpDenom);
		_mm_free (hpSumWt);
	}
	if (lp_sigma > 0) { //initialize high-pass reuseables
		_mm_free (lp);
		_mm_free (lpStart);
		_mm_free (lpEnd);
		_mm_free (lpSumWt);
	}
	return 0;
} // nifti_bptf()


static int nifti_demean(nifti_image * nim) {
	if (nim->datatype != DT_CALC) return 1;
	size_t nvox3D = nim->nx * nim->ny * MAX(1,nim->nz);
	if (nvox3D < 1) return 1; 
	int nvol = nim->nvox / nvox3D;
	if ((nvox3D * nvol) != nim->nvox) return 1;
	if (nvol < 1) {
		niimath_message("demean requires 4D datasets\n");
		return 1;
	}
	flt * img = (flt *) nim->data;
	#pragma omp parallel for
	for (size_t i = 0; i < nvox3D; i++) {
		double sum = 0.0;
		for (size_t v = i; v < nim->nvox; v+= nvox3D)
			sum += img[v];
		double mean = sum / nvol;
		for (size_t v = i; v < nim->nvox; v+= nvox3D)
			img[v] -= mean;
	}
	return 0;
}

static int nifti_dim_reduce(nifti_image * nim, enum eDimReduceOp op, int dim, int percentage) {
//e.g. nifti_dim_reduce(nim, Tmean, 4) reduces 4th dimension, saving mean
	int nReduce = nim->dim[dim];
	if ((nReduce <= 1) || (dim < 1) || (dim > 4)) return 0; //nothing to reduce, fslmaths does not generate an error
	if ((nim->nvox < 1) || (nim->nx < 1) || (nim->ny < 1) || (nim->nz < 1)) return 1;
	//size_t nvox3D = nim->nx * nim->ny * nim->nz;
	//int nvol = nim->nvox / nvox3D;
	//if ((nvox3D * nvol) != nim->nvox) return 1;
	if (nim->datatype != DT_CALC) return 1;
	if (nim->dim[0] > 4) 
		niimath_message("dimension reduction collapsing %"PRId64"D into to 4D\n", nim->dim[0]);
	int dims[8], indims[8];
	for (int i = 0; i < 4; i++ )
		dims[i] = MAX(nim->dim[i],1);
	//XYZT limits to 4 dimensions, so collapse dims [4,5,6,7]
	dims[4] =  nim->nvox / (dims[1]*dims[2]*dims[3]);
	for (int i = 5; i < 8; i++ )
		dims[i] = 1;
	for (int i = 0; i < 8; i++ )
		indims[i] = dims[i];
	if ((dims[1]*dims[2]*dims[3]*dims[4]) != nim->nvox)
		return 1; //e.g. data in dim 5..7!
	dims[dim] = 1;
	if (dim == 4) dims[0] = 3; //reduce 4D to 3D
	size_t nvox = dims[1]*dims[2]*dims[3]*dims[4];	
	flt * i32 = (flt *) nim->data;
	void * dat = (void *)calloc(1,nim->nvox * sizeof(flt)) ;
	flt * o32 = (flt *) dat;
	int collapseStep; //e.g. if we collapse 4th dimension, we will collapse across voxels separated by X*Y*Z
	if (dim == 1)
		collapseStep = 1; //collapse by columns
	else if (dim == 2)
		collapseStep = indims[1]; //collapse by rows
	else if (dim == 3)
		collapseStep = indims[1]*indims[2]; //collapse by slices
	else
		collapseStep = indims[1]*indims[2]*indims[3]; //collapse by volumes
	int xy = dims[1]*dims[2];
	int xyz = xy * dims[3];
	if ((op == Tmedian) || (op == Tstd) || (op == Tperc) || (op == Tar1)) {
		//for even number of items, two options for median, consider 4 volumes ranked
		// meam of 2nd and 3rd: problem one can return values not in data
		// 2nd value. Representative
		//here we use the latter approach
		//int itm = ((nReduce-1) * 0.5);
		int itm = (nReduce * 0.5); //seems correct tested with odd and even number of volumes
		if (op == Tperc) {
			double frac = ((double)percentage)/100.0;
			//itm = ((nReduce-1) * frac);
			itm = ((nReduce) * frac);
			itm = MAX(itm, 0);
			itm = MIN(itm, nReduce-1);
		}
		#pragma omp parallel for
		for (size_t i = 0; i < nvox; i++ ) {
			flt * vxls = (flt *)_mm_malloc((nReduce)*sizeof(flt), 64); 
			size_t inPos = i;
			if (dim < 4) { //i is in output space, convert to input space, allows single loop for OpenMP
				int T = (i / xyz); //volume
				int r = i % (xyz);
				int Z = (r / xy); //slice
				r = r % (xy);
				int Y = (r / dims[1]); //row
				int X = r % dims[1];
				inPos = X+(Y*indims[1])+(Z*indims[1]*indims[2])+(T*indims[1]*indims[2]*indims[3]);
			}
			for (int v = 0; v < nReduce; v++ ) {
				vxls[v] = i32[inPos];
				inPos += collapseStep;
			}
			if ((op == Tstd) || (op == Tar1)) {
				//computed in cache, far fewer operations than Welford
				//note 64-bit double precision even if 32-bit DT_CALC
				//neither precision gives identical results
				// double precision attenuates catastrophic cancellation
				double sum = 0.0;
				for (int v = 0; v < nReduce; v++ )
					sum += vxls[v];
				double mean = sum / nReduce;
				double sumSqr = 0.0;
				for (int v = 0; v < nReduce; v++ )
					sumSqr += sqr(vxls[v]- mean);
				if (op == Tstd) 
					o32[i] = sqrt(sumSqr / (nReduce - 1));
				else { //Tar1
					if (sumSqr == 0.0) {
						o32[i] = 0.0;
						continue;
					}
					for (int v = 0; v < nReduce; v++ )
						vxls[v] = vxls[v] - mean; //demean
					double r = 0.0;
					for (int v = 1; v < nReduce; v++ )
						r += (vxls[v] * vxls[v-1])/sumSqr; 
					o32[i] = r;
				}
			} else { //Tperc or Tmedian
				qsort (vxls, nReduce, sizeof(flt), compare);
				o32[i] = vxls[itm];
			}
			_mm_free (vxls);			
		} //for i: each voxel	
	} else {
		#pragma omp parallel for
		for (size_t i = 0; i < nvox; i++ ) {
			size_t inPos = i; //ok if dim==4
			if (dim < 4) { //i is in output space, convert to input space, allows single loop for OpenMP
				int T = (i / xyz); //volume
				int r = i % (xyz);
				int Z = (r / xy); //slice
				r = r % (xy);
				int Y = (r / dims[1]); //row
				int X = r % dims[1];
				inPos = X+(Y*indims[1])+(Z*indims[1]*indims[2])+(T*indims[1]*indims[2]*indims[3]);
			} 
			double sum = 0.0;
			flt mx = i32[inPos];
			flt mn = mx;
			int mxn = 0;
			//flt sd = 0.0;
			//flt mean = 0.0;
			for (int v = 0; v < nReduce; v++ ) {
				flt f = i32[inPos];
				sum += f;
				if (f > mx) {
					mx = f;
					mxn = v;
				}
				mn = MIN(mn, f);
				//Welford https://en.wikipedia.org/wiki/Algorithms_for_calculating_variance
				// 2-pass method faster
				//flt delta = f - mean;
				//mean = mean + delta / (v+1);
				//sd = sd + delta*(f- mean);
				inPos += collapseStep;
			}
			if (op == Tmean)
				o32[i] = sum / nReduce; //mean
			else if (op == Tmax)
				o32[i] = mx; //max
			else if (op == Tmaxn)
				o32[i] = mxn; //maxn
			else if (op == Tmin)
				o32[i] = mn; //min

		}
	} //if opel
	nim->nvox = nvox;
	for (int i = 0; i < 4; i++ )
		nim->dim[i] = dims[i];
	nim->ndim = dims[0];
	nim->nx = dims[1];
	nim->ny = dims[2];
	nim->nz = dims[3];
	nim->nt = dims[4];
	nim->nu = dims[5];
	nim->nv = dims[6];
	nim->nw = dims[7]; 
	free(nim->data);
	nim->data = dat;
	return 0;
} //Tar1

enum eOp{unknown, add, sub, mul, divX, rem, mod, mas, thr, thrp, thrP, uthr, uthrp, uthrP, 
	max, min, 
	power,
	seed, inm, ing, smth,
	exp1,log1,sin1,cos1,tan1,asin1,acos1,atan1,sqr1,sqrt1,recip1,abs1,bin1,binv1,edge1, index1,
	nan1, nanm1, rand1, randn1,range1, rank1, ranknorm1,
	pval1, pval01, cpval1,
	ztop1, ptoz1,
	dilMk,dilDk,dilFk,dilallk,erok,eroFk,fmediank,fmeank,fmeanuk,
	subsamp2,subsamp2offc
	};
	
static int * make_kernel_gauss(nifti_image * nim, int * nkernel, double sigmamm) {
	sigmamm = fabs(sigmamm);
	if (sigmamm == 0.0) return NULL;
	double mmCutoff = sigmamm * 6.0; //maximum extent
	int x = (2*floor(mmCutoff/nim->dx))+1;
	int y = (2*floor(mmCutoff/nim->dy))+1;
	int z = (2*floor(mmCutoff/nim->dz))+1;
	int xlo = (int)(-x / 2);
	int ylo = (int)(-y / 2);
	int zlo = (int)(-z / 2);
	//betterthanfsl
	// fsl computes gaussian for all values in cube
	// from first principles, a spherical filter has less bias
	// since weighting is very low at these edge voxels, it has little impact on
	// "-fmean", however with other filters like "dilM", fsl's solution works like 
	// a "box" filter, not a "sphere" filter
	// default is to clone fsl 
	#ifdef betterthanfsl //true sphere at cutouff
	//first pass: determine number of surviving voxels (n)
	int n = 0; 
	for (int zi = zlo; zi < (zlo+z); zi++ )
		for (int yi = ylo; yi < (ylo+y); yi++ )
			for (int xi = xlo; xi < (xlo+x); xi++ ) {
				flt dx = (xi * nim->dx);
				flt dy = (yi * nim->dy);
				flt dz = (zi * nim->dz);
				flt dist = sqrt(dx*dx + dy*dy + dz*dz);
				if (dist > mmCutoff) continue;
				n++;
			}
	*nkernel = n;
	int kernelWeight = (int)((double)INT_MAX/(double)n); //requires <limits.h>
	int * kernel = (int *)_mm_malloc((n*4)*sizeof(int), 64); //4 values: offset, xpos, ypos, weight
	double * wt = (double *)_mm_malloc((n)*sizeof(double), 64); //precess weight: temporary
	//second pass: fill surviving voxels
	int i = 0;
	double expd = 2.0*sigmamm*sigmamm;
	for (int zi = zlo; zi < (zlo+z); zi++ )
		for (int yi = ylo; yi < (ylo+y); yi++ )
			for (int xi = xlo; xi < (xlo+x); xi++ ) {
				flt dx = (xi * nim->dx);
				flt dy = (yi * nim->dy);
				flt dz = (zi * nim->dz);
				flt dist = sqrt(dx*dx + dy*dy + dz*dz);
				if (dist > mmCutoff) continue;
				kernel[i] = xi + (yi * nim->nx) + (zi * nim->nx * nim->ny);
				kernel[i+n] = xi; //left-right wrap detection
				kernel[i+n+n] = yi; //anterior-posterior wrap detection
				//kernel[i+n+n+n] = kernelWeight; //kernel height
				wt[i] = exp(-1.0*(dist*dist)/expd);
				i++;
			}
	#else
	int n = x * y * z; 
	*nkernel = n;
	int * kernel = (int *)_mm_malloc((n*4)*sizeof(int), 64); //4 values: offset, xpos, ypos, weight
	double * wt = (double *)_mm_malloc((n)*sizeof(double), 64); //precess weight: temporary
	int i = 0;
	double expd = 2.0*sigmamm*sigmamm;
	for (int zi = zlo; zi < (zlo+z); zi++ )
		for (int yi = ylo; yi < (ylo+y); yi++ )
			for (int xi = xlo; xi < (xlo+x); xi++ ) {
				flt dx = (xi * nim->dx);
				flt dy = (yi * nim->dy);
				flt dz = (zi * nim->dz);
				flt dist = sqrt(dx*dx + dy*dy + dz*dz);
				//if (dist > mmCutoff) continue; //<- fsl fills all
				kernel[i] = xi + (yi * nim->nx) + (zi * nim->nx * nim->ny);
				kernel[i+n] = xi; //left-right wrap detection
				kernel[i+n+n] = yi; //anterior-posterior wrap detection
				//kernel[i+n+n+n] = kernelWeight; //kernel height
				wt[i] = exp(-1.0*(dist*dist)/expd);
				i++;
			}	
	#endif
	double sum = 0.0;
	for (int i = 0; i < n; i++ )
		sum += wt[i];
	//sum of entire gaussian is 1
	double scale = 1.0 / sum;
	scale *= (double)INT_MAX; //we use integer scaling: in future faster to typecast integer as flt (if int=32bit) or double (if int=64bit) 
	for (int i = 0; i < n; i++ )
		kernel[i+n+n+n] = wt[i]*scale;
	_mm_free (wt);
	return kernel;
} //make_kernel_gauss()

static flt calmax(nifti_image * nim){
	if ((nim->nvox < 1) || (nim->datatype != DT_CALC)) return 0.0;
	flt * in32 = (flt *) nim->data;
	flt mx = in32[0];
	for (size_t i = 0; i < nim->nvox; i++ )
		mx = MAX(mx, in32[i]);
	return mx;
}
	
static flt calmin(nifti_image * nim){
	if ((nim->nvox < 1) || (nim->datatype != DT_CALC)) return 0.0;
	flt * in32 = (flt *) nim->data;
	flt mn = in32[0];
	for (size_t i = 0; i < nim->nvox; i++ )
		mn = MIN(mn, in32[i]);
	return mn;
}

/*void swapSign(nifti_image * nim){
	if ((nim->nvox < 1) || (nim->datatype != DT_CALC)) return;
	flt * in32 = (flt *) nim->data;
	for (size_t i = 0; i < nim->nvox; i++ )
		in32[i] = -in32[i];
}*/

static int nifti_tensor_2(nifti_image * nim, int lower2upper) {
	int nvox3D = nim->dim[1] * nim->dim[2] * nim->dim[3];
	if ((nim->datatype != DT_CALC) || (nvox3D < 1)) return 1;
	int nVol = (int)(nim->nvox/nvox3D);
	if (nVol != 6) {
		niimath_message("nifti_tensor_2: input must have precisely 6 volumes (not %d)\n", nVol);
		return 1;	
	}
	//3dAFNItoNIFTI does not set intent_code to NIFTI_INTENT_SYMMATRIX, so check dimensions
	if ((lower2upper) && (nim->dim[4] == 6))
		niimath_message("nifti_tensor_2: check images (header suggests already in upper triangle format)\n");
	if ((!lower2upper) && (nim->dim[4] == 6))
		niimath_message("nifti_tensor_2: check images (header suggests already in lower triangle format)\n");
			
	//lower xx xy yy xz yz zz
	//upper xx xy xz yy yz zz
	//swap volumes 3 and 4
	flt * in32 = (flt *) nim->data;
	flt * tmp = (flt *)_mm_malloc(nvox3D*sizeof(flt), 64);
	flt * v3 = in32 + (2 * nvox3D);
	flt * v4 = in32 + (3 * nvox3D);
	memcpy(tmp, v4, nvox3D*sizeof(flt));
	memcpy(v4, v3, nvox3D*sizeof(flt));
	memcpy(v3, tmp, nvox3D*sizeof(flt));
	_mm_free (tmp);
	if (lower2upper) { 
		//FSL uses non-standard upper triangle
		nim->dim[0] = 4;
		for (int i = 4; i < 8; i++)
			nim->dim[i] = 1;
		nim->dim[4] = 6;
		nim->ndim = 4;
		nim->nt = 6;
		nim->nu = 1;
		nim->nv = 1;
		nim->nw = 1;  	
	} else { //upper2lower
		//lower is NIfTI default, used by AFNI, Camino, ANTS
		nim->intent_code = NIFTI_INTENT_SYMMATRIX;
		/*! To store an NxN symmetric matrix at each voxel:
		- dataset must have a 5th dimension
		- intent_code must be NIFTI_INTENT_SYMMATRIX
		- dim[5] must be N*(N+1)/2
		- intent_p1 must be N (in float format)
		- the matrix values A[i][[j] are stored in row-order:
		- A[0][0]
		- A[1][0] A[1][1]
		- A[2][0] A[2][1] A[2][2]
		- etc.: row-by-row                           */
		nim->dim[0] = 5;
		for (int i = 4; i < 8; i++)
			nim->dim[i] = 1;
		nim->dim[5] = 6;
		nim->ndim = 5;
		nim->nt = 1;
		nim->nu = 6;
		nim->nv = 1;
		nim->nw = 1;   
    }
	return 0;
}

static int nifti_tensor_decomp(nifti_image * nim, int isUpperTriangle) {
// MD= (Dxx+Dyy+Dzz)/3
//https://github.com/ANTsX/ANTs/wiki/Importing-diffusion-tensor-data-from-other-software
// dtifit produces upper-triangular order: xx xy xz yy yz zz
//MD = 1/3*(Dxx+Dyy+Dzz)
//FA= sqrt(3/2)*sqrt(((Dx-MD)^2+(Dy-MD)^2+(Dz-MD^2))/(Dx^2+Dy^2+Dz^2))
//fslmaths tensor.nii -tensor_decomp bork.nii
// 3dDTeig -uddata -sep_dsets -prefix AFNIdwi.nii tensor.nii
//3dDTeig expects LOWER diagonal order unless -uddata
// Dxx,Dxy,Dyy,Dxz,Dyz,Dzz
// https://afni.nimh.nih.gov/pub/dist/doc/program_help/3dDTeig.html
//dxx, dxy, dyy, dxz, dyz, dzz
// 3dDTeig -uddata -prefix AFNIdwi.nii tensor.nii
// fslmaths tensor.nii -tensor_decomp bork.nii
// Creates 5*3D and 3*4D files for a total of 14 volumes L1,L2,L3,V1(3),V2(3),V3(3),FA,MD
#ifdef tensor_decomp
	if ((nim->nvox < 1) || (nim->nx < 2) || (nim->ny < 2) || (nim->nz < 1)) return 1;
	if (nim->datatype != DT_CALC) return 1;
	int nvox3D = nim->dim[1] * nim->dim[2] * nim->dim[3];
	int nVol = (int)(nim->nvox/nvox3D);
	if (nVol != 6) {
		niimath_message("nifti_tensor_decomp: input must have precisely 6 volumes (not %d)\n", nVol);
		return 1;	
	}
	flt * in32 = (flt *) nim->data;
	//detect if data is upper or lower triangle
	//  The "YY" component should be brighter (stronlgy positive) than the off axis XZ
	#define detectUpperOrLower
	#ifdef detectUpperOrLower
	double sumV3 = 0.0; //3rd volume, YY for lower, XZ for upper
	double sumV4 = 0.0; //4th volume, XZ for lower, YY for upper
	flt * v32 = in32 + (nvox3D * 2); //offset to 3rd volume
	for (size_t i = 0; i < nvox3D; i++ )
			sumV3 += v32[i];	
	v32 = in32 + (nvox3D * 3); //offset to 4th volume
	for (size_t i = 0; i < nvox3D; i++ )
			sumV4 += v32[i];
	if ((sumV4 > sumV3) && (!isUpperTriangle))
		niimath_message("nifti_tensor_decomp: check results, input looks like UPPER triangle.\n");
	if ((sumV4 < sumV3) && (isUpperTriangle))
		niimath_message("nifti_tensor_decomp: check results, input looks like LOWER triangle.\n");
	#endif
	flt * out32 = (flt *)_mm_malloc(14*nvox3D*sizeof(flt), 64);
	for (size_t i = 0; i < nvox3D; i++ ) {
		//n.b. in6 and out14 are ALWAYS float regradless of DT32, e.g. single even if DT=double
		float * in6 = (float *)_mm_malloc(6*sizeof(float), 64);
		float * out14 = (float *)_mm_malloc(14*sizeof(float), 64);
		size_t iv = i;
		for (int v = 0; v < 6; v++) {
			in6[v] = in32[iv];
			iv += nvox3D;
		}
		EIG_tsfunc(0.0, 0.0, 0, in6, 0.0, 0.0, NULL, 0, out14, isUpperTriangle); 
		size_t ov = i;
		for (int v = 0; v < 14; v++) {
			out32[ov] = out14[v];
			ov += nvox3D;
		}
		_mm_free (out14);	
		_mm_free (in6);	
	}
	free(nim->data);
	// Creates 5*3D and 3*4D files for a total of 14 volumes L1(0),L2(1),L3(2),V1(3,4,5),V2(6,7,8),V3(9,10,11),FA(12),MD(13)
	flt * outv;
	//save 4D images
	nim->cal_min = -1;
	nim->cal_max = 1;
	nim->nvox = nvox3D * 3;
	nim->ndim = 4;
	nim->nt = 3;
	nim->nu = 1;
	nim->nv = 1;
	nim->nw = 1;  
	nim->dim[0] = 4;
	nim->dim[4] = 3;
	for (int i = 5; i < 8; i++)
		nim->dim[i] = 1;
	//void * dat = (void *)calloc(1, 3*nvox3D * sizeof(flt)) ;
	//nim->data = dat;
	//flt * fa32 = (flt *) dat;	
	//save V1
	outv = out32 + (nvox3D * 3);
	//memcpy(fa32, outv, 3*nvox3D*sizeof(flt)); 
	/*for (size_t i = 0; i < (3*nvox3D); i++ )
		if (outv[i] != 0.0) // do not create "-0.0"
			outv[i] = -outv[i];	*/
	nim->data = (void *)outv;	
	nifti_save(nim, "_V1");
	//save V2
	outv = out32 + (nvox3D * 6);
	//memcpy(fa32, outv, 3*nvox3D*sizeof(flt)); 
	nim->data = (void *)outv;
	nifti_save(nim, "_V2");
	//save V3
	outv = out32 + (nvox3D * 9);
	//memcpy(fa32, outv, 3*nvox3D*sizeof(flt)); 
	nim->data = (void *)outv;
	nifti_save(nim, "_V3");
	//release 4D memory
	//free(dat);
	//save 3D images
	nim->cal_min = 0;
	nim->cal_max = 0;
	nim->nvox = nvox3D * 1;
	nim->ndim = 3;
	nim->nt = 1;
	nim->dim[0] = 3;
	nim->dim[4] = 1;
	//save L1
	outv = out32;
	//memcpy(fa32, outv, nvox3D*sizeof(flt)); 
	nim->data = (void *)outv;
	nim->cal_max = calmax(nim);
	nifti_save(nim, "_L1");
	//save L2
	outv = out32 + (nvox3D * 1);
	//memcpy(fa32, outv, nvox3D*sizeof(flt)); 
	nim->data = (void *)outv;
	nim->cal_max = calmax(nim);
	nifti_save(nim, "_L2");
	//save L3
	outv = out32 + (nvox3D * 2);
	//memcpy(fa32, outv, nvox3D*sizeof(flt)); 
	nim->data = (void *)outv;
	nim->cal_max = calmax(nim);
	nifti_save(nim, "_L3");
	//save MD
	outv = out32 + (nvox3D * 13);
	//memcpy(fa32, outv, nvox3D*sizeof(flt)); 
	nim->data = (void *)outv;
	nim->cal_min = calmin(nim);
	nim->cal_max = calmax(nim);
	nifti_save(nim, "_MD");
	//single volume data
	void * dat = (void *)calloc(1, nvox3D * sizeof(flt)) ;
	nim->data = dat;
	flt * fa32 = (flt *) dat;
	//save MO
	//MODE https://www.jiscmail.ac.uk/cgi-bin/webadmin?A2=FSL;4fbed3d1.1103
	// compute MO (MODE) from L1, L2, L3, MD
	//e1=l1-MD, e2=l2-MD, e3=l3-MD;
	//n = (e1 + e2 - 2*e3)*(2*e1 - e2 - e3)*(e1 - 2*e2 + e3);
	//d = (e1*e1 + e2*e2 + e3*e3 - e1*e2 - e2*e3 - e1*e3);
	//d = 2*d*d*d;
	//mode = n/d;
	//something is wrong with this formula.
	// a. Ennis 2006 includes a sqrt that can not be factored out
	// b. results differ from fslmaths
	nim->cal_min = -1;
	nim->cal_max = 1;	   
	flt * L1 = out32;
	flt * L2 = out32 + (nvox3D * 1);
	flt * L3 = out32 + (nvox3D * 2);
	flt * MD = out32 + (nvox3D * 13);
	for (size_t i = 0; i < nvox3D; i++ ) {
		flt e1 = L1[i] - MD[i];
		flt e2 = L2[i] - MD[i];
		flt e3 = L3[i] - MD[i];
		flt n = (e1 + e2 - 2*e3)*(2*e1 - e2 - e3)*(e1 - 2*e2 + e3);
		flt d = (e1*e1 + e2*e2 + e3*e3 - e1*e2 - e2*e3 - e1*e3);
		d = sqrt(d); //Correlation r = 0.999746
		d = 2*d*d*d;
		//d = sqrt(d); //Correlation r = 0.990319
		if (d != 0)  d = n / d; //mode
		d = MIN(d, 1.0);
		d = MAX(d, -1.0);
		fa32[i] = d;
	}
	nifti_save(nim, "_MO");
	//save FA
	outv = out32 + (nvox3D * 12);
	memcpy(fa32, outv, nvox3D*sizeof(flt)); 
	nim->cal_min = 0;
	nim->cal_max = 1;
	nifti_save(nim, "_FA");
	//keep FA in memory
	nim->cal_max = 0;
	_mm_free (out32);
	return 0;
#else
		niimath_message("not compiled to support tensor_decomp\n");
		return 1;
#endif
} //nifti_tensor_decomp()

static void kernel3D_dilall( nifti_image * nim, int * kernel, int nkernel, int vol) {
	int nVox3D = nim->dim[1] * nim->dim[2] * nim->dim[3];
	flt * f32 = (flt *) nim->data;
	f32 += (nVox3D * vol);
	flt * inf32 = (flt *)_mm_malloc(nVox3D*sizeof(flt), 64);
	memcpy(inf32, f32, nVox3D*sizeof(flt));
	int nxy = nim->nx * nim->ny;
	size_t nZero = 1;
	while (nZero > 0) {
		nZero = 0;
		for (int z = 0; z < nim->nz; z++ ) {
			int i = (z * nxy) -1; //offset
			for (int y = 0; y < nim->ny; y++ ) {
				for (int x = 0; x < nim->nx; x++ ) {
					i++;
					if (f32[i] != 0.0) continue;
					int nNot0 = 0;
					flt sum = 0.0f;
					for (size_t k = 0; k < nkernel; k++) {
						size_t vx = i + kernel[k];
						if ((vx < 0) || (vx >= nVox3D) || (inf32[vx] == 0.0)) continue;
						//next handle edge cases
						int dx = x+kernel[k+nkernel];
						if ((dx < 0) || (dx >= nim->nx)) continue; //wrapped left-right
						int dy = y+kernel[k+nkernel+nkernel];
						if ((dy < 0) || (dy >= nim->ny)) continue; //wrapped anterior-posterior
						nNot0 ++;
						sum += inf32[vx];
					} //for k
					if (nNot0 > 0) f32[i] = sum / nNot0;
					nZero++;
				} //for x
			} //for y
		} //for z
		memcpy(inf32, f32, nVox3D*sizeof(flt));
		//niimath_print("n=0: %zu\n", nZero);
	} //nZero > 0
	_mm_free (inf32);
} //kernel3D_dilall()

static int kernel3D( nifti_image * nim, enum eOp op, int * kernel, int nkernel, int vol) {
	int nVox3D = nim->dim[1] * nim->dim[2] * nim->dim[3];
	flt * f32 = (flt *) nim->data;
	f32 += (nVox3D * vol);
	flt * inf32 = (flt *)_mm_malloc(nVox3D*sizeof(flt), 64);
	memcpy(inf32, f32, nVox3D*sizeof(flt));
	int nxy = nim->nx * nim->ny;
	if (op == fmediank) {
		flt * vxls = (flt *)_mm_malloc((nkernel)*sizeof(flt), 64); 
		for (int z = 0; z < nim->nz; z++ ) {
			int i = (z * nxy) -1; //offset
			for (int y = 0; y < nim->ny; y++ ) {
				for (int x = 0; x < nim->nx; x++ ) {
					i++;
					int nOK = 0;
					for (size_t k = 0; k < nkernel; k++) {
						size_t vx = i + kernel[k];
						if ((vx < 0) || (vx >= nVox3D)) continue;
						//next handle edge cases
						int dx = x+kernel[k+nkernel];
						if ((dx < 0) || (dx >= nim->nx)) continue; //wrapped left-right
						int dy = y+kernel[k+nkernel+nkernel];
						if ((dy < 0) || (dy >= nim->ny)) continue; //wrapped anterior-posterior
						vxls[nOK] = inf32[vx];
						nOK ++;
					} //for k
					qsort (vxls, nOK, sizeof(flt), compare);
					int itm = (nOK * 0.5); 
					f32[i] = vxls[itm];
				} //for x
			} //for y
		} //for z	
		_mm_free (vxls);
	} else if (op == dilMk) {
		for (int z = 0; z < nim->nz; z++ ) {
			int i = (z * nxy) -1; //offset
			for (int y = 0; y < nim->ny; y++ ) {
				for (int x = 0; x < nim->nx; x++ ) {
					i++;
					if (f32[i] != 0.0) continue;
					int nNot0 = 0;
					flt sum = 0.0f;
					for (size_t k = 0; k < nkernel; k++) {
						size_t vx = i + kernel[k];
						if ((vx < 0) || (vx >= nVox3D) || (inf32[vx] == 0.0)) continue;
						//next handle edge cases
						int dx = x+kernel[k+nkernel];
						if ((dx < 0) || (dx >= nim->nx)) continue; //wrapped left-right
						int dy = y+kernel[k+nkernel+nkernel];
						if ((dy < 0) || (dy >= nim->ny)) continue; //wrapped anterior-posterior
						nNot0 ++;
						sum += inf32[vx];
					} //for k
					if (nNot0 > 0) f32[i] = sum / nNot0;
				} //for x
			} //for y
		} //for z
	} else if (op == dilDk){ //maximum - fslmaths 6.0.1 emulation, note really MODE, max non-zero
		for (int z = 0; z < nim->nz; z++ ) {
			int i = (z * nxy) -1; //offset
			for (int y = 0; y < nim->ny; y++ ) {
				for (int x = 0; x < nim->nx; x++ ) {
					i++;
					if (f32[i] != 0.0) continue;
					//flt mx = -INFINITY;
					flt mx = NAN;
					for (int k = 0; k < nkernel; k++) {
						int vx = i + kernel[k];
						if ((vx < 0) || (vx >= nVox3D)) continue;
						//next handle edge cases
						int dx = x+kernel[k+nkernel];
						if ((dx < 0) || (dx >= nim->nx)) continue; //wrapped left-right
						int dy = y+kernel[k+nkernel+nkernel];
						if ((dy < 0) || (dy >= nim->ny)) continue; //wrapped anterior-posterior
						flt v = inf32[vx];
						if (v == 0.0) continue;
						mx = MAX(mx,inf32[vx]);
						//with dilD a input voxel of 0 	
					} //for k
					//https://stackoverflow.com/questions/570669/checking-if-a-double-or-float-is-nan-in-c
					//  f != f will be true only if f is NaN
					if (!(mx != mx))
						f32[i] = mx; 
					
				} //for x
			} //for y
		} //for z
	} else if (op == dilFk)  { //maximum - fslmaths 6.0.1 appears to use "dilF" when the user requests "dilD"
		for (int z = 0; z < nim->nz; z++ ) {
			int i = (z * nxy) -1; //offset
			for (int y = 0; y < nim->ny; y++ ) {
				for (int x = 0; x < nim->nx; x++ ) {
					i++;
					flt mx = f32[i];
					for (int k = 0; k < nkernel; k++) {
						int vx = i + kernel[k];
						if ((vx < 0) || (vx >= nVox3D) || (inf32[vx] <= mx)) continue;
						//next handle edge cases
						int dx = x+kernel[k+nkernel];
						if ((dx < 0) || (dx >= nim->nx)) continue; //wrapped left-right
						int dy = y+kernel[k+nkernel+nkernel];
						if ((dy < 0) || (dy >= nim->ny)) continue; //wrapped anterior-posterior
						mx = MAX(mx,inf32[vx]);
						//if (mx < 0) continue; //with dilF, do not make a zero voxel darker than 0
						
					} //for k
					f32[i] = mx; 
				} //for x
			} //for y
		} //for z	
	} else if (op == dilallk) {//	-dilall : Apply -dilM repeatedly until the entire FOV is covered");
		kernel3D_dilall(nim, kernel, nkernel, vol);	
	} else if (op == eroFk) { //Minimum filtering of all voxels
		for (int z = 0; z < nim->nz; z++ ) {
			int i = (z * nxy) -1; //offset
			for (int y = 0; y < nim->ny; y++ ) {
				for (int x = 0; x < nim->nx; x++ ) {
					i++;
					for (int k = 0; k < nkernel; k++) {
						int vx = i + kernel[k];
						if ((vx < 0) || (vx >= nVox3D)) continue;
						//next handle edge cases
						int dx = x+kernel[k+nkernel];
						if ((dx < 0) || (dx >= nim->nx)) continue; //wrapped left-right
						int dy = y+kernel[k+nkernel+nkernel];
						if ((dy < 0) || (dy >= nim->ny)) continue; //wrapped anterior-posterior
						f32[i] = MIN(f32[i], inf32[vx]);
					
					} //for k
				} //for x
			} //for y
		} //for z
	} else if (op == fmeank) {
		flt * kwt = (flt *)_mm_malloc(nkernel*sizeof(flt), 64);
		for (int k = 0; k < nkernel; k++)
			kwt[k] = ((double)kernel[k+nkernel+nkernel+nkernel]/(double)INT_MAX ); 
		for (int z = 0; z < nim->nz; z++ ) {
			int i = (z * nxy) -1; //offset
			for (int y = 0; y < nim->ny; y++ ) {
				for (int x = 0; x < nim->nx; x++ ) {
					i++;
					flt sum = 0.0f;
					flt wt = 0.0f;
					for (int k = 0; k < nkernel; k++) {
						int vx = i + kernel[k];
						if ((vx < 0) || (vx >= nVox3D)) continue;
						//next handle edge cases
						int dx = x+kernel[k+nkernel];
						if ((dx < 0) || (dx >= nim->nx)) continue; //wrapped left-right
						int dy = y+kernel[k+nkernel+nkernel];
						if ((dy < 0) || (dy >= nim->ny)) continue; //wrapped anterior-posterior
						sum += (inf32[vx]* kwt[k]);
						wt += kwt[k];
					} //for k
					f32[i] = sum / wt;
				} //for x
			} //for y
		} //for z
		_mm_free (kwt);	
	} else if (op == fmeanuk) {
		flt * kwt = (flt *)_mm_malloc(nkernel*sizeof(flt), 64);
		for (int k = 0; k < nkernel; k++)
			kwt[k] = ((double)kernel[k+nkernel+nkernel+nkernel]/(double)INT_MAX ); 
		for (int z = 0; z < nim->nz; z++ ) {
			int i = (z * nxy) -1; //offset
			for (int y = 0; y < nim->ny; y++ ) {
				for (int x = 0; x < nim->nx; x++ ) {
					i++;
					flt sum = 0.0f;
					//flt wt = 0.0f;
					for (int k = 0; k < nkernel; k++) {
						int vx = i + kernel[k];
						if ((vx < 0) || (vx >= nVox3D)) continue;
						//next handle edge cases
						int dx = x+kernel[k+nkernel];
						if ((dx < 0) || (dx >= nim->nx)) continue; //wrapped left-right
						int dy = y+kernel[k+nkernel+nkernel];
						if ((dy < 0) || (dy >= nim->ny)) continue; //wrapped anterior-posterior
						sum += (inf32[vx]* kwt[k]);
						//wt += kwt[k];
					} //for k
					//f32[i] = sum / wt;
					f32[i] = sum;
				} //for x
			} //for y
		} //for z
		_mm_free (kwt);
	} else if (op == erok) {
		for (int z = 0; z < nim->nz; z++ ) {
			int i = (z * nxy) -1; //offset
			for (int y = 0; y < nim->ny; y++ ) {
				for (int x = 0; x < nim->nx; x++ ) {
					i++;
					if (f32[i] == 0.0) continue;
					for (int k = 0; k < nkernel; k++) {
						int vx = i + kernel[k];
						if ((vx < 0) || (vx >= nVox3D) || (inf32[vx] != 0.0)) continue;
						//next handle edge cases
						int dx = x+kernel[k+nkernel];
						if ((dx < 0) || (dx >= nim->nx)) continue; //wrapped left-right
						int dy = y+kernel[k+nkernel+nkernel];
						if ((dy < 0) || (dy >= nim->ny)) continue; //wrapped anterior-posterior
						f32[i] = 0.0;
					
					} //for k
				} //for x
			} //for y
		} //for z	
	} else {
		niimath_message("kernel3D: Unsupported operation\n");
		_mm_free (inf32);
		return 1;
	} 
	_mm_free (inf32);
	return 0;
} //kernel3D

static int nifti_kernel ( nifti_image * nim, enum eOp op, int * kernel, int nkernel) {
	if ((nim->nvox < 1) || (nim->nx < 2) || (nim->ny < 2) || (nim->nz < 1)) return 1;
	if (nim->datatype != DT_CALC) return 1;
	int nVox3D = nim->dim[1] * nim->dim[2] * nim->dim[3];
	int nVol = (int)(nim->nvox/nVox3D);
	if (nVol < 1) return 1;
	if ((nkernel < 1) || (kernel == NULL)) return 1;
	for (int v = 0; v < nVol; v++ ) {
		int ok = kernel3D(nim, op, kernel, nkernel, v);
		if (ok != 0) return ok;
	}
	return 0;
}

static int nifti_roi ( nifti_image * nim, int xmin, int xsize, int ymin, int ysize, int zmin, int zsize, int tmin, int tsize) {
// "fslmaths LAS -roi 3 32 0 40 0 40 0 5 f "
	int nt = nim->nvox / (nim->nx * nim->ny * nim->nz);
	if ((nim->nvox < 1) || (nt < 1)) return 1;
	if (nim->datatype != DT_CALC) return 1;
	flt * f32 = (flt *) nim->data;
	//if (neg_determ(nim)) 
	//	do something profound; //determinants do not seem to influence "-roi"?
	int xmax = xmin + xsize - 1;
	int ymax = ymin + ysize - 1;
	int zmax = zmin + zsize - 1;
	int tmax = tmin + tsize - 1;
	//niimath_print("%d..%d", zmin, zmax);
	size_t i = 0;
	for (int t = 0; t < nt; t++) {
		int tOK = 1;
		if ((t < tmin) || (t > tmax)) tOK = 0; 
		for (int z = 0; z < nim->nz; z++) {
			int zOK = 1;
			if ((z < zmin) || (z > zmax)) zOK = 0; 
			for (int y = 0; y < nim->ny; y++) {
				int yOK = 1;
				if ((y < ymin) || (y > ymax)) yOK = 0; 
				for (int x = 0; x < nim->nx; x++) {
					int xOK = 1;
					if ((x < xmin) || (x > xmax)) xOK = 0; 
					if ((xOK == 0) || (yOK == 0) || (zOK == 0) || (tOK == 0))
						f32[i] = 0.0;
					i++;				
				} //x
			} //y
		} //z
	}//t
	return 0;				
}

static int nifti_sobel( nifti_image * nim, int offc) { 
//sobel is simply one kernel pass per dimension.
// this could be achieved with successive passes of "-kernel"
// here it is done in a single pass for cache efficiency
// https://en.wikipedia.org/wiki/Sobel_operator
	int vox3D = nim->nx*nim->ny*MAX(nim->nz,1);
	if (nim->datatype != DT_CALC) return 1;
	int nvol = nim->nvox/vox3D; 
	int numk = 6;//center voxel and all its neighbors
	int * kx  = (int *)_mm_malloc((numk*4)*sizeof(int), 64); //4 values: offset, xpos, ypos, weight
	int * ky  = (int *)_mm_malloc((numk*4)*sizeof(int), 64); //4 values: offset, xpos, ypos, weight
	int * kz  = (int *)_mm_malloc((numk*4)*sizeof(int), 64); //4 values: offset, xpos, ypos, weight
	int i = 0;
	for (int x = 0; x <= 1; x++)
		for (int y = -1; y <= 1; y++) {
			int sgn = (2*x)-1; //-1 or +1
			int weight = sgn * (2 - abs(y)); 
			//kx compare left and right
			kx[i+numk] = (2*x)-1; //left/right wrap
			kx[i+numk+numk] = y; //anterior/posterior wrap
			kx[i] = kx[i+numk] + (kx[i+numk+numk] * (nim->nx)); //voxel offset
			kx[i+numk+numk+numk] = weight; //weight
			//ky compare anterior and posterior
			ky[i+numk] = y; //left/right wrap
			ky[i+numk+numk] = (2*x)-1; //anterior/posterior wrap
			ky[i] = ky[i+numk] + (ky[i+numk+numk] * (nim->nx)); //voxel offset
			ky[i+numk+numk+numk] = weight; //weight
			//kz superior/inferior
			kz[i+numk] = y; //left/right wrap
			kz[i+numk+numk] = 0; //anterior/posterior wrap
			kz[i] = y + (((2*x)-1) *  nim->nx * nim->ny); //voxel offset
			kz[i+numk+numk+numk] = weight; //weight	
			//niimath_print("x%d y%d wt%d\n", kx[i+numk], kx[i+numk+numk], kx[i+numk+numk+numk]);
			//niimath_print("x%d y%d wt%d\n", ky[i+numk], ky[i+numk+numk], ky[i+numk+numk+numk]);
			i++;	
		} //for y
	flt * i32 = (flt *) nim->data; //input volumes
	#pragma omp parallel for
	for (int v = 0; v < nvol; v++) {
		flt * iv32 = i32 + (v * vox3D);
		flt * imgin = _mm_malloc(vox3D*sizeof(flt), 64); //input values prior to blur	
		memcpy(imgin, iv32, vox3D*sizeof(flt));
		int i = 0;
		for (int z = 0; z < nim->nz; z++ )
			for (int y = 0; y < nim->ny; y++ )
				for (size_t x = 0; x < nim->nx; x++ ) {
					//compute z gradient
					flt gx = 0.0f;
					for (size_t k = 0; k < numk; k++) {
						size_t vx = i + kx[k];
						if ((vx < 0) || (vx >= vox3D)) continue;
						//next handle edge cases
						int dx = x+kx[k+numk];
						if ((dx < 0) || (dx >= nim->nx)) continue; //wrapped left-right
						int dy = y+kx[k+numk+numk];
						if ((dy < 0) || (dy >= nim->ny)) continue; //wrapped anterior-posterior
						gx += imgin[vx] * kx[k+numk+numk+numk] ;
					} //for k
					//compute y gradient
					flt gy = 0.0f;
					for (size_t k = 0; k < numk; k++) {
						size_t vx = i + ky[k];
						if ((vx < 0) || (vx >= vox3D)) continue;
						//next handle edge cases
						int dx = x+ky[k+numk];
						if ((dx < 0) || (dx >= nim->nx)) continue; //wrapped left-right
						int dy = y+ky[k+numk+numk];
						if ((dy < 0) || (dy >= nim->ny)) continue; //wrapped anterior-posterior
						gy += imgin[vx] * ky[k+numk+numk+numk] ;
					} //for k
					//compute z gradient
					flt gz = 0.0f; //always 0 for 2D, we could add conditional to skip but optimize for 3D
					for (size_t k = 0; k < numk; k++) {
						size_t vx = i + kz[k];
						if ((vx < 0) || (vx >= vox3D)) continue;
						//next handle edge cases
						int dx = x+kz[k+numk];
						if ((dx < 0) || (dx >= nim->nx)) continue; //wrapped left-right
						int dy = y+kz[k+numk+numk];
						if ((dy < 0) || (dy >= nim->ny)) continue; //wrapped anterior-posterior
						gz += imgin[vx] * kz[k+numk+numk+numk] ;
					} //for k				
					iv32[i] = sqrt(sqr(gx)+sqr(gy)+sqr(gz));
					i++;
				} //for x
		_mm_free (imgin);
	}
	_mm_free (kx);
	_mm_free (ky);
	_mm_free (kz);
	return 0;
} //nifti_sobel()

static int nifti_subsamp2 ( nifti_image * nim, int offc) {
	//naive downsampling: this is provided purely to mimic the behavior of fslmaths
	// see https://nbviewer.jupyter.org/urls/dl.dropbox.com/s/s0nw827nc4kcnaa/Aliasing.ipynb
    // no anti-aliasing filter  https://en.wikipedia.org/wiki/Image_scaling
	int invox3D = nim->nx*nim->ny*MAX(nim->nz,1);
	int indim[5];
	for (int i = 1; i < 5; i++)
		indim[i] = MAX(nim->dim[i],1);
	int nvol = nim->nvox/invox3D;
	int x_odd = indim[1] % 2;
	if ((nim->nvox < 1) || (nvol < 1)) return 1;
	if (nim->datatype != DT_CALC) return 1;
	int nx = ceil(nim->nx * 0.5);
	int ny = ceil(nim->ny * 0.5);
	int nz = ceil(nim->nz * 0.5);
	if ((nx == nim->nx) && (ny == nim->ny) && (nz == nim->nz)) return 0;
	int nvox3D = nx*ny*nz;
	flt * i32 = (flt *) nim->data;
	void * dat = (void *)calloc(1, nvox3D * nvol * sizeof(flt)) ;
	flt * o32 = (flt *) dat;
	int x_flip = 0;
	if (!neg_determ(nim))
		x_flip = 1;
	if (offc) {
		int * wt = _mm_malloc(nvox3D * nvol *sizeof(int), 64); //weight, just for edges	
		for (int i = 0; i < (nvox3D * nvol); i++) { 
			wt[i] = 0;
			o32[i] = 0.0;
		}
		int boost = 0;
		if ((x_odd) && (x_flip)) 
			boost = 1;
		size_t i = 0;
		for (int v = 0; v < indim[4]; v++) {
			size_t vo = v * nvox3D; //volumes do not get reduced
			for (int z = 0; z < indim[3]; z++) {
				size_t zo = vo + ((z / 2) * ny * nx);	
				for (int y = 0; y < indim[2]; y++) {
					size_t yo = zo + ((y / 2) * nx);
					for (int x = 0; x < indim[1]; x++) {
						size_t xo = yo + ((x+boost) / 2) ;
						wt[xo]++;
						o32[xo] += i32[i];
						i++;
					
					} //x
				}//y
			}//z
		}//vol	
		for (int i = 0; i < (nvox3D * nvol); i++)
			if (wt[i] > 0)
				o32[i] /= wt[i];
		_mm_free (wt);	
	} else { //if subsamp2offc else subsamp2
		int numk = 27;//center voxel and all its neighbors
		int * kernel  = (int *)_mm_malloc((numk*4)*sizeof(int), 64); //4 values: offset, xpos, ypos, weight
		int i = 0;
		for (int z = -1; z <= 1; z++ )
			for (int y = -1; y <= 1; y++ )
				for (int x = -1; x <= 1; x++ ) {
				kernel[i] = x + (y * indim[1]) + (z * indim[1] * indim[2]);
				kernel[i+numk] = x; //left-right wrap detection
				kernel[i+numk+numk] = y; //anterior-posterior wrap detection
				kernel[i+numk+numk+numk] = 8/(pow(2,sqr(x)+sqr(y)+sqr(z))); //kernel weight
				i++;
			}
		int boost = 0;
		//if ((xflip == 1) && (odd == 0)) boost = 1;
		if ((x_flip == 1) && (x_odd == 0)) boost = 1; 
		//niimath_print("boost %d\n", boost);
		size_t nvox3Din = indim[1]*indim[2]*indim[3];
		size_t o = 0;	
		for (int v = 0; v < nvol; v++) {
			size_t vi = v * nvox3Din;
			for (int z = 0; z < nz; z++) {
				int zi = (2 * z * indim[1] *indim[2]);	
				//niimath_print("%zu \n", zi);
				for (int y = 0; y < ny; y++) {
					int yy = y+y; //y*2 input y
					int yi = zi + (yy * indim[1]);
					for (int x = 0; x < nx; x++) {
						//int xx = x+x+xflip; //x*2 input x
						int xx = x+x+boost; //x*2 input x
						int xi = yi + xx;
						//flt sum = 0.0;
						//flt wt = 0.0;
						double sum = 0.0;
						double wt = 0.0;
						for (int k = 0; k < numk; k++ ) {
							if  ((xi+kernel[k]) < 0) continue; //position would be less than 0 - outside volume, avoid negative values in size_t
							size_t pos = xi + kernel[k]; //offset
							if (pos >= nvox3Din) continue; //position outside volume, e.g. slice above top of volume
							int xin = xx+kernel[k+numk];
							if ((xin < 0) || (xin >= indim[1])) continue; //wrap left or right
							int yin = yy+kernel[k+numk+numk];
							if ((yin < 0) || (yin >= indim[2])) continue; //wrap anterior or posterior
							flt w = kernel[k+numk+numk+numk];
							wt += w; 
							sum += i32[vi+pos] * w;
						}
						//if (wt > 0.0) //no need to check: every voxel has at least one contributor (itself)
							o32[o] = sum/wt;
						//else {
						//	o32[o] =  666.6;
						o++;
						
					} //x
				}//y
			}//z
		}//vol
		_mm_free (kernel);			
	} //if subsamp2offc else subsamp2 
	nim->nvox = nvox3D * nvol;
	nim->nx = nx;
	nim->ny = ny;
	nim->nz = nz;
	nim->dim[1] = nx;
	nim->dim[2] = ny;
	nim->dim[3] = nz;
	nim->dx *= 2;
	nim->dy *= 2;
	nim->dz *= 2;
	nim->pixdim[1] *= 2;
	nim->pixdim[2] *= 2;
	nim->pixdim[3] *= 2;
	//adjust origin
	mat44 m = xform(nim);
	vec4 vx = setVec4(0,0,0);
	vec4 pos = nifti_vect44mat44_mul(vx, m);
	//vx = setVec4(0.5,0.5,0.5);
	//vx = setVec4(1.0,0.0,0.0);
	if (offc) {
		//niimath_print("%d flip odd %d\n", x_flip, x_odd);
		if ((x_odd) && (x_flip)) 
			vx = setVec4(-0.5,-0.5,-0.5); //subsamp2offc
		else
			vx = setVec4(0.5,0.5,0.5); //subsamp2offc
		//if (!xflip) {
		//	vx = setVec4(0.5,0.5,0.5);
		//	niimath_print("y\n");	
		//}
	} else {
		if (x_odd)
			vx = setVec4(0,0,0); //subsamp2
		else
			vx = setVec4(1,0,0); //subsamp2
		if (!x_flip)
			vx = setVec4(0,0,0);
	}
	vec4 pos1 =  nifti_vect44mat44_mul(vx, m);
	vx = setVec4(pos1.v[0]-pos.v[0], pos1.v[1]-pos.v[1], pos1.v[2]-pos.v[2]);
	m.m[0][3] += vx.v[0];
	m.m[1][3] += vx.v[1];
	m.m[2][3] += vx.v[2];
	//scale spatial transform
	for (int i = 0; i < 3; i++)
		for (int j = 0; j < 3; j++) 
			m.m[i][j] *= 2;
	//apply to both sform and qform in case VTK user
	for (int i = 0; i < 4; i++)
		for (int j = 0; j < 4; j++) {
			nim->sto_xyz.m[i][j] = m.m[i][j];
			nim->qto_xyz.m[i][j] = m.m[i][j];
		}		
	free(nim->data);
	nim->data = dat;
	return 0;
}

static int nifti_resize ( nifti_image * nim, flt zx, flt zy, flt zz, int interp_method) {
	//see AFNI's 3dresample 
	//better than fslmaths: fslmaths can not resample 4D data
	// time 3dresample -dxyz 4.8 4.8 4.8 -rmode Linear -prefix afni.nii -input rest.nii
	// time ./sm rest.nii -subsamp2 out.nii
	//However, aliasing artifacts
	// time 3dresample -dxyz 4.8 4.8 4.8 -rmode Linear -prefix afni2.nii -input zoneplate3d_129.nii 
	int invox3D = nim->nx*nim->ny*nim->nz;
	int nvol = nim->nvox/invox3D;
	if ((nim->nvox < 1) || (nvol < 1)) return 1;
	if (nim->datatype != DT_CALC) return 1;
	int nx = ceil(nim->nx * zx);
	int ny = ceil(nim->ny * zy);
	int nz = ceil(nim->nz * zz);
	if ((nx == nim->nx) && (ny == nim->ny) && (nz == nim->nz)) return 0;
	int nvox3D = nx*ny*nz;
	flt * i32 = (flt *) nim->data;
	void * dat = (void *)calloc(1, nvox3D * nvol * sizeof(flt)) ;
	flt * o32 = (flt *) dat;
	#pragma omp parallel for
	for (int v = 0; v < nvol; v++) {
		flt * iv32 = i32 + (v * invox3D);
		//reduce in X: half the width: 1/2 input file size
		flt * imgx = _mm_malloc(nx*nim->ny*nim->nz*sizeof(flt), 64); //input values prior to blur	
		if (nx == nim->nx) //no change in x dimension
			memcpy(imgx, iv32, nx*nim->ny*nim->nz*sizeof(flt));
		else {
			CLIST * contrib = createFilter(nim->nx, nx, interp_method);
			size_t i = 0;
			for (size_t y = 0; y < (nim->ny * nim->nz); y++) {
				for (int x = 0; x < nx; x++) {
					flt weight = 0.0;
					for (int j = 0; j < contrib[x].n; j++) 
						weight += iv32[contrib[x].p[j].pixel]* contrib[x].p[j].weight;
					imgx[i++] = weight;
				}
				iv32 += nim->nx;
			} //for y
			for (i = 0; i < nx; i++)
				free(contrib[i].p);
			free(contrib);
		}
		//reduce in Y: half the height: 1/4 input size
		flt * imgy = _mm_malloc(nx*ny*nim->nz*sizeof(flt), 64); //input values prior to blur	
		if (ny == nim->ny) //no change in y dimension
			memcpy(imgy, imgx, nx*ny*nim->nz*sizeof(flt));
		else {
			CLIST * contrib = createFilter(nim->ny, ny, interp_method);
			flt * iny = _mm_malloc(nim->ny*sizeof(flt), 64); //input values prior to resize		
			for (int z = 0; z < nim->nz; z++) {
				for (int x = 0; x < nx; x++) {
					int yo = (z * nx * ny) + x; //output
					int yi = (z * nx * nim->ny) + x;//input
					for (int j = 0; j < nim->ny; j++) {
							//iny[j] = imgx[yi+(j*nx)];
							iny[j] = imgx[yi];
							yi += nx;
						}
					for (int y = 0; y < ny; y++) {
						flt weight = 0.0;
						for (int j = 0; j < contrib[y].n; j++) 
							weight += iny[contrib[y].p[j].pixel]* contrib[y].p[j].weight;
						//weight = y;
						imgy[yo] = weight;
						yo += nx;
					} //y
				} //x
			} //z
			_mm_free (iny);
			for (int i = 0; i < ny; i++)
				free(contrib[i].p);
			free(contrib);
		}
		_mm_free (imgx);
		//reduce in Z
		flt * ov32 = o32 + (v * nvox3D);
		if (nz == nim->nz) //no change in x dimension
			memcpy(ov32, imgy, nx*ny*nz*sizeof(flt));
		else {
			CLIST * contrib = createFilter(nim->nz, nz, interp_method);
			flt * inz = _mm_malloc(nim->nz*sizeof(flt), 64); //input values prior to resize
			int nxy = nx * ny;
			for (int y = 0; y < ny; y++) {
				for (int x = 0; x < nx; x++) {
					int zo = x + (y * nx); //output offset
					int zi = x + (y * nx); //input offset
					for (int j = 0; j < nim->nz; j++) {
						inz[j] = imgy[zi];
						zi += nxy;
					}
					for (int z = 0; z < nz; z++) {
						//for (int j = 0; j < nim->nz; j++)
						//	inz[j] = imgy[zi+(j*nx*ny)];
						flt weight = 0.0;
						for (int j = 0; j < contrib[z].n; j++) 
							weight += inz[contrib[z].p[j].pixel]* contrib[z].p[j].weight;
						//weight = y;
						ov32[zo] = weight;
						zo += nx*ny;
					} //for z
				} //for x
			} //for y		
			_mm_free (inz);
			for (int i = 0; i < nz; i++)
				free(contrib[i].p);
			free(contrib);
		}
		_mm_free (imgy);
	} //for v	
	nim->nvox = nvox3D * nvol;
	nim->nx = nx;
	nim->ny = ny;
	nim->nz = nz;
	nim->dim[1] = nx;
	nim->dim[2] = ny;
	nim->dim[3] = nz;
	nim->dx /= zx;
	nim->dy /= zy;
	nim->dz /= zz;
	nim->pixdim[1] /= zx;
	nim->pixdim[2] /= zy;
	nim->pixdim[3] /= zz;
	//adjust origin - again, just like fslmaths
	mat44 m = xform(nim);
	/*vec4 vx = setVec4(0,0,0); 
	vec4 pos = nifti_vect44mat44_mul(vx, m);
	vx = setVec4(0.5,0.5,0.5); //subsamp2offc
	vx = setVec4(1,0,0); //subsamp2
	vec4 pos1 =  nifti_vect44mat44_mul(vx, m);
	vx = setVec4(pos1.v[0]-pos.v[0], pos1.v[1]-pos.v[1], pos1.v[2]-pos.v[2]);
	m.m[0][3] += vx.v[0];
	m.m[1][3] += vx.v[1];
	m.m[2][3] += vx.v[2];*/
	m.m[0][0] /= zx;
	m.m[1][0] /= zx;
	m.m[2][0] /= zx;
	m.m[0][1] /= zy;
	m.m[1][1] /= zy;
	m.m[2][1] /= zy;
	m.m[0][2] /= zz;
	m.m[1][2] /= zz;
	m.m[2][2] /= zz;
	for (int i = 0; i < 4; i++) //transform BOTH sform and qform (e.g. ANTs/ITK user)
		for (int j = 0; j < 4; j++) {
			nim->sto_xyz.m[i][j] = m.m[i][j];
			nim->qto_xyz.m[i][j] = m.m[i][j];
		}	
	free(nim->data);
	nim->data = dat;
	return 0;
}

static int essentiallyEqual(float a, float b) {
	if (isnan(a) && isnan(b)) return 1; //surprisingly, with C nan != nan
    return fabs(a - b) <= ( (fabs(a) > fabs(b) ? fabs(b) : fabs(a)) * epsilon);
}

#ifndef USING_R
static void nifti_compare(nifti_image * nim, char * fin) {
	if (nim->nvox < 1) exit( 1);
	if (nim->datatype != DT_CALC) {
		niimath_message("nifti_compare: Unsupported datatype %d\n", nim->datatype);
		exit( 1);
	}
	nifti_image * nim2 = nifti_image_read2(fin, 1);
	if( !nim2 ) {
	  niimath_message("** failed to read NIfTI image from '%s'\n", fin);
	  exit(2);
	}
	if ((nim->nx != nim2->nx) || (nim->ny != nim2->ny) || (nim->nz != nim2->nz) ) {
		niimath_message("** Attempted to process images of different sizes %"PRId64"x%"PRId64"x%"PRId64"vs %"PRId64"x%"PRId64"x%"PRId64"\n", nim->nx,nim->ny,nim->nz, nim2->nx,nim2->ny,nim2->nz);
		nifti_image_free( nim2 );
		exit(1);
	}
	if (nim->nvox != nim2->nvox) {
		niimath_message(" Number of volumes differ\n");
		nifti_image_free( nim2 );
		exit(1);
	}
	if (max_displacement_mm(nim, nim2) > 0.5) { //fslmaths appears to use mm not voxel difference to determine alignment, threshold ~0.5mm
		niimath_message("WARNING:: Inconsistent orientations for individual images in pipeline! (%gmm)\n", max_displacement_mm(nim, nim2));
		niimath_message(" Will use voxel-based orientation which is probably incorrect - *PLEASE CHECK*!\n");
	}
	in_hdr ihdr = set_input_hdr(nim2);
	if (nifti_image_change_datatype(nim2, nim->datatype, &ihdr) != 0) {
		nifti_image_free( nim2 );
		exit(1);
	}
	flt * img = (flt *) nim->data;
	flt * img2 = (flt *) nim2->data;
	size_t differentVox = nim->nvox;
	double sum = 0.0;
	double sum2 = 0.0;
	double maxDiff = 0.0;
	size_t nNotNan = 0;
	size_t nDifferent = 0;
	for (size_t i = 0; i < nim->nvox; i++ ) {
		if (!essentiallyEqual(img[i], img2[i])) {
			if (fabs(img[i]-img2[i]) > maxDiff) {
				differentVox = i;
				maxDiff = fabs(img[i]-img2[i]);
			}
			nDifferent ++; 
		}
		if (isnan(img[i]) ||  isnan(img[i]) )
			continue;
		nNotNan++;
		sum += img[i];
		sum2 += img2[i];
	}
	if (differentVox >= nim->nvox) {
		//niimath_message("Images essentially equal\n"); */
		nifti_image_free( nim2 );
		exit(0);
	}
	//second pass - one pass correlation is inaccurate or slow
	nNotNan = MAX(1, nNotNan);
	flt mn = INFINITY; //do not set to item 1, in case it is nan
	flt mx = -INFINITY;
	flt sd = 0.0;
	flt ave = sum /  nNotNan;
	flt mn2 = INFINITY;
	flt mx2 = -INFINITY;
	flt sd2 = 0.0;
	flt ave2 = sum2 /  nNotNan;
 	 //for i := 0 to (n - 1) do
     // sd := sd + sqr(y[i] - mn);
  	//sd := sqrt(sd / (n - 1));
	double sumDx = 0.0;
	for (size_t i = 0; i < nim->nvox; i++ ) {
		if (isnan(img[i]) ||  isnan(img[i]) )
			continue;
		mn = MIN(mn, img[i]);
		mx = MAX(mx, img[i]);
		sd += sqr(img[i] - ave);
		mn2 = MIN(mn2, img2[i]);
		mx2 = MAX(mx2, img2[i]);
		sd2 += sqr(img2[i] - ave2);
				sumDx += (img[i] - ave)*(img2[i] - ave2);
	}
	double r = 0.0;
	nNotNan = MAX(2, nNotNan);
	if (nim->nvox < 2) {
		sd = 0.0;
		sd2 = 0.0;
	} else {
		sd = sqrt(sd / (nNotNan - 1));
  		//if (sd != 0.0)  sd = 1.0/sd;
		sd2 = sqrt(sd2 / (nNotNan - 1));
  		//if (sd2 != 0.0)  sd2 = 1.0/sd2;
  		if ((sd * sd2) != 0.0) 
  			r = sumDx/(sd*sd2*(nNotNan - 1));
  		//r = r / (nim->nvox - 1);
	}
	r = MIN(r,1.0);
	r = MAX(r, -1.0);
	niimath_message("Images Differ: Correlation r = %g, identical voxels %d%%\n", r, (int)floor(100.0*(1.0-(double)nDifferent/(double)nim->nvox)));
	if (nNotNan < nim->nvox) {
		niimath_message("  %"PRId64" voxels have a NaN in at least one image.\n", nim->nvox - nNotNan);
		niimath_message("  Descriptives consider voxels that are numeric in both images.\n");
	}
	niimath_message("  Most different voxel %g vs %g (difference %g)\n", img[differentVox], img2[differentVox], maxDiff);
	int nvox3D = nim->nx * nim->ny * MAX(nim->nz,1);
	int nVol = nim->nvox/nvox3D;
	size_t vx[4];
	vx[3] = differentVox/nvox3D;
	vx[2] = (differentVox / (nim->nx*nim->ny)) % nim->nz;
	vx[1] = (differentVox / nim->nx) % nim->ny;
	vx[0] = differentVox % nim->nx;	
	niimath_message("  Most different voxel locatoin %zux%zux%zu volume %zu\n", vx[0],vx[1],vx[2], vx[3]);
	niimath_message("Image 1 Descriptives\n");
	niimath_message(" Range: %g..%g Mean %g StDev %g\n", mn, mx, ave, sd);
	niimath_message("Image 2 Descriptives\n");
	niimath_message(" Range: %g..%g Mean %g StDev %g\n", mn2, mx2, ave2, sd2);
	//V1 comparison - EXIT_SUCCESS if all vectors are parallel (for DWI up vector [1 0 0] has same direction as down [-1 0 0])
	if (nVol != 3) {
		nifti_image_free( nim2 );
		exit(1);
	}
	int allParallel = 1;
	//niimath ft_V1 -compare nt_V1
	for (size_t i = 0; i < nvox3D; i++ ) {
		//check angle of two vectors... assume unit vectors
		flt v[3]; //vector, image 1
		v[0] = img[i];
		v[1] = img[i+nvox3D];
		v[2] = img[i+nvox3D+nvox3D];
		flt v2[3]; //vector, image 2
		v2[0] = img2[i];
		v2[1] = img2[i+nvox3D];
		v2[2] = img2[i+nvox3D+nvox3D];
		flt x[3]; //cross product
		x[0] = (v[1]*v2[2]) - (v[2]*v2[1]);
		x[1] = (v[2]*v2[0]) - (v[0]*v2[2]);
		x[2] = (v[0]*v2[1]) - (v[1]*v2[0]);
		flt len = sqrt((x[0]*x[0])+(x[1]*x[1])+(x[2]*x[2]));
		if (len > 0.01) {
			allParallel = 0;
			//niimath_message("[%g %g %g] vs [%g %g %g]\n", v[0],v[1], v[2], v2[0], v2[1], v2[2]);
			break;
		}
	}
	if ( allParallel ) {
		niimath_message("Despite polarity differences, all vectors are parallel.\n");
		nifti_image_free( nim2 );
		exit(0);
	}
	nifti_image_free( nim2 );
	exit(1);
} //nifti_compare()
#endif // USING_R

static int nifti_binary_power ( nifti_image * nim, double v) {
	//clone operations from ANTS ImageMath: power
	//https://manpages.debian.org/jessie/ants/ImageMath.1.en.html
	if (nim->nvox < 1) return 1;
	if (nim->datatype!= DT_CALC) return 1;
	flt fv = v;
	flt * f32 = (flt *) nim->data;
	for (size_t i = 0; i < nim->nvox; i++ )
			f32[i] = pow(f32[i], v);
		return 0;
}

static int nifti_binary ( nifti_image * nim, char * fin, enum eOp op) {
	if (nim->nvox < 1) return 1;
	if (nim->datatype != DT_CALC) {
		niimath_message("nifti_binary: Unsupported datatype %d\n", nim->datatype);
		return 1;
	}
	nifti_image * nim2 = nifti_image_read2(fin, 1);
	if( !nim2 ) {
	  niimath_message("** failed to read NIfTI image from '%s'\n", fin);
	  return 2;
	}
	if ((nim->nx != nim2->nx) || (nim->ny != nim2->ny) || (nim->nz != nim2->nz) ) {
		niimath_message("** Attempted to process images of different sizes %"PRId64"x%"PRId64"x%"PRId64" vs %"PRId64"x%"PRId64"x%"PRId64"\n", nim->nx,nim->ny,nim->nz, nim2->nx,nim2->ny,nim2->nz);
		nifti_image_free( nim2 );
		return 1;
	}
	if (max_displacement_mm(nim, nim2) > 0.5) { //fslmaths appears to use mm not voxel difference to determine alignment, threshold ~0.5mm
		niimath_message("WARNING:: Inconsistent orientations for individual images in pipeline! (%gmm)\n", max_displacement_mm(nim, nim2));
		niimath_message(" Will use voxel-based orientation which is probably incorrect - *PLEASE CHECK*!\n");
	}
	in_hdr ihdr = set_input_hdr(nim2);
	if (nifti_image_change_datatype(nim2, nim->datatype, &ihdr) != 0) {
		nifti_image_free( nim2 );
		return 1;
	}
	flt * imga = (flt *) nim->data;
	flt * imgb = (flt *) nim2->data;
	int nvox3D = nim->nx * nim->ny * nim->nz;	
	int nvola = nim->nvox / nvox3D;
	int nvolb = nim2->nvox / nvox3D;
	int rem0 = 0;
	int swap4D = 0; //if 1: input nim was 3D, but nim2 is 4D: output will be 4D
	if ((nvolb > 1) && (nim->nvox != nim2->nvox) && ((op == uthr) || (op == thr))) {
		//"niimath 3D -uthr 4D out" only uses 1st volume of 4D, only one volume out
		nvolb = 1; //fslmaths 
		niimath_print("threshold operation expects 3D mask\n"); //fslmaths makes not modification to image
		if (op == uthr) //strictly for fslmaths compatibility - makes no sense
			for (size_t i = 0; i < nim->nvox; i++ )
				imga[i] = 0;
		nifti_image_free( nim2 );
		return 0;
	} else if (nim->nvox != nim2->nvox) {
		//situation where one input is 3D and the other is 4D
		if ((nvola != 1) && ((nvolb != 1))) {
			niimath_message("nifti_binary: both images must have the same number of volumes, or one must have a single volume (%d and %d)\n", nvola, nvolb);
			nifti_image_free( nim2 );
			return 1;	
		}
		if (nvola == 1) {
			imgb = (flt *) nim->data;
			imga = (flt *) nim2->data;		
			swap4D = 1;
			nvolb = nim->nvox / nvox3D;
			nvola = nim2->nvox / nvox3D;
		}	
	} //make it so imga/novla >= imgb/nvolb
	for (int v = 0; v < nvola; v++ ) { //
		int va = v * nvox3D; //start of volume for image A
		int vb = (v % nvolb) * nvox3D; //start of volume for image B
		if (op == add) {
			for (int i = 0; i < nvox3D; i++ )
				imga[va+i] += imgb[vb+i];
		} else if (op == sub) {
			if (swap4D) {
				for (int i = 0; i < nvox3D; i++ ) {
						imga[va+i] = imgb[vb+i] - imga[va+i];
					//niimath_print(">>[%d]/[%d] %g/%g = %g\n",vb+i, va+i,  imgb[vb+i], x, imga[va+i]); 
				}
			} else { 
				for (int i = 0; i < nvox3D; i++ ) {
					//niimath_print("[%d]/[%d] %g/%g\n", va+i, vb+i, imga[va+i], imga[vb+i]); 
						imga[va+i] = imga[va+i] - imgb[vb+i];
				}
			}	
		} else if (op == mul) {
			for (int i = 0; i < nvox3D; i++ )
				imga[va+i] *= imgb[vb+i];
		} else if (op == max) {
			for (int i = 0; i < nvox3D; i++ )
				imga[va+i] = MAX(imga[va+i], imgb[vb+i]);
		} else if (op == min) {
			for (int i = 0; i < nvox3D; i++ )
				imga[va+i] = MIN(imga[va+i], imgb[vb+i]);
		} else if (op == thr) {
			//thr   : use following number to threshold current image (zero anything below the number)
			for (int i = 0; i < nvox3D; i++ )
				if (imga[va+i] < imgb[vb+i])
					imga[va+i] = 0;
		} else if (op == uthr) {
			//uthr  : use following number to upper-threshold current image (zero anything above the number)
			for (int i = 0; i < nvox3D; i++ )
				if (imga[va+i] > imgb[vb+i])
					imga[va+i] = 0;
			
		} else if (op == mas) {
			if (swap4D) {
				for (int i = 0; i < nvox3D; i++ ) {
					if (imga[va+i] > 0)
						imga[va+i] = imgb[vb+i];
					else
						imga[va+i] = 0;
				}
			} else {
				for (int i = 0; i < nvox3D; i++ )
					if (imgb[vb+i] <= 0)
						imga[va+i] = 0;
			}
		} else if (op == divX) {
			if (swap4D) {
				for (int i = 0; i < nvox3D; i++ ) {
					//flt x = imga[va+i];
					if (imga[va+i] != 0.0f)
						imga[va+i] = imgb[vb+i]/imga[va+i];
					//niimath_print(">>[%d]/[%d] %g/%g = %g\n",vb+i, va+i,  imgb[vb+i], x, imga[va+i]); 
				}
			} else { 
				for (int i = 0; i < nvox3D; i++ ) {
					//niimath_print("[%d]/[%d] %g/%g\n", va+i, vb+i, imga[va+i], imga[vb+i]); 
					if (imgb[vb+i] == 0.0f)
						imga[va+i] = 0.0f;
					else
						imga[va+i] = imga[va+i]/imgb[vb+i];
				}
			}
		} else if (op == mod) { //afni mod function, divide by zero yields 0 (unlike Matlab, see remtest.m)
			//fractional remainder:
			if (swap4D) {
				for (int i = 0; i < nvox3D; i++ ) {
					//niimath_print("!>[%d]/[%d] %g/%g = %g\n",vb+i, va+i,  imgb[vb+i], imga[va+i], fmod(trunc(imgb[vb+i]), trunc(imga[va+i]))   );
					if (imga[va+i] != 0.0f)
						imga[va+i] = fmod(imgb[vb+i], imga[va+i]);
					else {
						rem0 = 1;
						imga[va+i] = 0;//imgb[vb+i];
					}
					 
				}
			} else { 
				for (int i = 0; i < nvox3D; i++ ) {
					//niimath_print("?>[%d]/[%d] %g/%g = %g : %g\n", va+i, vb+i, imga[va+i], imgb[vb+i], fmod(imga[va+i], imgb[vb+i]), fmod(trunc(imga[va+i]), trunc(imgb[vb+i]))  ); 
					if (imgb[vb+i] != 0.0f)
						//imga[va+i] = round(fmod(imga[va+i], imgb[vb+i]));
						imga[va+i] = fmod(imga[va+i], imgb[vb+i]);
					else {
						rem0 = 1;
						imga[va+i] = 0;	
					}
				}
			}
		} else if (op == rem) { //fmod _rem
			//fractional remainder:
			if (swap4D) {
				for (int i = 0; i < nvox3D; i++ ) {
					//niimath_print("!>[%d]/[%d] %g/%g = %g\n",vb+i, va+i,  imgb[vb+i], imga[va+i], fmod(trunc(imgb[vb+i]), trunc(imga[va+i]))   );
					if (trunc(imga[va+i]) != 0.0f)
						imga[va+i] = fmod(trunc(imgb[vb+i]), trunc(imga[va+i]));
					else {
						rem0 = 1;
						imga[va+i] = imgb[vb+i];
					}
					 
				}
			} else { 
				for (int i = 0; i < nvox3D; i++ ) {
					//niimath_print("?>[%d]/[%d] %g/%g = %g : %g\n", va+i, vb+i, imga[va+i], imgb[vb+i], fmod(imga[va+i], imgb[vb+i]), fmod(trunc(imga[va+i]), trunc(imgb[vb+i]))  ); 
					if (trunc(imgb[vb+i]) != 0.0f)
						//imga[va+i] = round(fmod(imga[va+i], imgb[vb+i]));
						imga[va+i] = fmod(trunc(imga[va+i]), trunc(imgb[vb+i]));
					else
						rem0 = 1;	
				}
			}	
		} else {
			niimath_message("nifti_binary: unsupported operation %d\n", op);
			nifti_image_free( nim2 );
			return 1;		
		}
	}
	if (swap4D) { //if 1: input nim was 3D, but nim2 is 4D: output will be 4D
		nim->nvox = nim2->nvox;
		nim->ndim = nim2->ndim;
		nim->nt =nim2->nt;
		nim->nu =nim2->nu;
		nim->nv =nim2->nv;
		nim->nw =nim2->nw;
		for (int i = 4; i < 8; i++ ) {
			nim->dim[i] =nim2->dim[i];
			nim->pixdim[i] =nim2->pixdim[i];
		}
		nim->dt =nim2->dt;
		nim->du =nim2->du;
		nim->dv =nim2->dv;
		nim->dw =nim2->dw;
		free(nim->data);
		nim->data = nim2->data;
		nim2->data = NULL; 
	}
	nifti_image_free( nim2 );
	if (rem0) {
		niimath_message("Warning -rem image included zeros (fslmaths exception)\n");
		return 0;	
	}
	return 0;
} // nifti_binary()

struct sortIdx {
	flt val;
	int idx;
};

static int nifti_roc( nifti_image * nim, double fpThresh, const char * foutfile,  const char * fnoise, const char * ftruth) {
	if (nim->datatype != DT_CALC) return 1;
//(nim, thresh, argv[outfile], fnoise, argv[truth]);
	//fslmaths appears to ignore voxels on edge of image, and will crash with small images:
	// error: sort(): given object has non-finite elements
	//therefore, there is a margin ("border") around the volume
	int border = 5; //in voxels
	int mindim = border + border + 1; //e.g. minimum size has one voxel surrounded by border on each side
	if ((nim->nx < mindim) || (nim->ny < mindim) || (nim->nz < mindim)) {
		niimath_message("volume too small for ROC analyses\n");
		return 1;
	}
	if (nim->nvox > (nim->nx * nim->ny * nim->nz)) {
		niimath_message("ROC input should be 3D image (not 4D)\n"); //fslmaths seg faults 
		return 1;
	}
	if ((fpThresh <= 0.0) || (fpThresh >= 1.0)) {
		niimath_message("ROC  false-positive threshold should be between 0 and 1, not '%g'\n", fpThresh);
		return 1;
	}
	nifti_image * nimTrue = nifti_image_read2(ftruth, 1);
#ifndef USING_R
	if( !nimTrue ) {
	  niimath_message("** failed to read NIfTI image from '%s'\n", ftruth);
	  exit(2);
	}
#endif
	if ((nim->nx != nimTrue->nx) || (nim->ny != nimTrue->ny) || (nim->nz != nimTrue->nz) ) {
		niimath_message("** Truth image is the wrong size %"PRId64"x%"PRId64"x%"PRId64" vs %"PRId64"x%"PRId64"x%"PRId64"\n", nim->nx,nim->ny,nim->nz, nimTrue->nx,nimTrue->ny,nimTrue->nz);
		nifti_image_free( nimTrue );
#ifdef USING_R
        return 1;
#else
		exit(1);
#endif
	}
	if (nimTrue->nvox > (nimTrue->nx * nimTrue->ny * nimTrue->nz)) {
		niimath_message("ROC truth should be 3D image (not 4D)\n"); //fslmaths seg faults 
		return 1;
	}
	nifti_image * nimNoise = NULL;
	//count number of tests
	//If the truth image contains negative voxels these get excluded from all calculations
	int nTest = 0;
	int nTrue = 0;
	size_t i = 0;
	flt * imgTrue = (flt *) nimTrue->data; 
	flt * imgObs = (flt *) nim->data; 
	for (int z = 0; z < nim->nz; z++ )
		for (int y = 0; y < nim->ny; y++ )
			for (int x = 0; x < nim->nx; x++ ) {
				if ((imgTrue[i] >= 0) && (x >= border) && (y >= border) && (z >= border)
					 && (x < (nim->nx - border))  && (y < (nim->ny - border))  && (z < (nim->nz - border)) ) {
						nTest++;
						if (imgTrue[i] > 0) nTrue++;
					}
				i++;
			}
	if (nTest < 1) {
		niimath_message("** All truth voxels inside border are negative\n");
#ifdef USING_R
        return 1;
#else
		exit(1);
#endif
	}
	//niimath_print("%d %d = %d\n", nTrue, nFalse, nTest);
	if (nTest == nTrue)
		niimath_message("Warning: All truth voxels inside border are the same (all true or all false)\n");
	struct sortIdx * k = (struct sortIdx *)_mm_malloc(nTest*sizeof(struct sortIdx), 64); 
	//load the data
	nTest = 0;
	i = 0;
	for (int z = 0; z < nim->nz; z++ )
		for (int y = 0; y < nim->ny; y++ )
			for (int x = 0; x < nim->nx; x++ ) {
				if ((imgTrue[i] >= 0) && (x >= border) && (y >= border) && (z >= border)
					 && (x < (nim->nx - border))  && (y < (nim->ny - border))  && (z < (nim->nz - border)) ) {
						k[nTest].val = imgObs[i];
						k[nTest].idx = imgTrue[i] > 0;
						nTest++;
					}
				i++;
			}
	qsort(k, nTest, sizeof(struct sortIdx), compare);
	//for (int v = 0; v < nvol; v++ )
	//	f32[ k[v].idx ] = v + 1;
	//niimath_print("%d tests, intensity range %g..%g\n", nTest, k[0].val, k[nTest-1].val);
	FILE* txt = fopen(foutfile, "w+");
	flt threshold = k[nTest-1].val; //maximum observed intensity
	int bins = 1000; //step size: how often are results reported
	flt step = (threshold-k[0].val)/bins; //[max-min]/bins
	int fp = 0;
	int tp = 0;
	if (fnoise != NULL) {
		nimNoise = nifti_image_read2(fnoise, 1);
		if ((nim->nx != nimNoise->nx) || (nim->ny != nimNoise->ny) || (nim->nz != nimNoise->nz) ) {
			niimath_message("** Noise image is the wrong size %"PRId64"x%"PRId64"x%"PRId64" vs %"PRId64"x%"PRId64"x%"PRId64"\n", nim->nx,nim->ny,nim->nz, nimNoise->nx,nimNoise->ny,nimNoise->nz);
			nifti_image_free( nimTrue );
			nifti_image_free( nimNoise );
#ifdef USING_R
            return 1;
#else
			exit(1);
#endif
		}
		//Matlab script roc.m generates samples you can process with fslmaths.\
		// The fslmaths text file includes two additional columns of output not described by the help documentation
		// Appears to find maximum signal in each noise volume, regardless of whether it is a hit or false alarm.
		int nvox3D = nim->dim[1] * nim->dim[2] * nim->dim[3];
		int nvol = nimNoise->nvox / nvox3D;		
		if (nvol < 10) niimath_message("Warning: Noise images should include many volumes for estimating familywise error/\n");
		flt * imgNoise = (flt *) nimNoise->data; 
		flt * mxVox = (flt *)_mm_malloc(nvol*sizeof(flt), 64);
		for (int v = 0; v < nvol; v++ ) { //for each volume
			mxVox[v] = -INFINITY;
			size_t vo = v * nvox3D;
			size_t vi = 0;
			for (int z = 0; z < nim->nz; z++ )
				for (int y = 0; y < nim->ny; y++ )
					for (int x = 0; x < nim->nx; x++ ) {
						if ((imgTrue[vi] >= 0) && (x >= border) && (y >= border) && (z >= border)
							 && (x < (nim->nx - border))  && (y < (nim->ny - border))  && (z < (nim->nz - border)) )
								mxVox[v] = MAX(mxVox[v], imgNoise[vo+vi]);
						vi++;
					}
		} //for each volume		
		nifti_image_free( nimNoise );
		qsort (mxVox, nvol, sizeof(flt), compare);
		int idx =  nTest - 1;
		flt mxNoise = mxVox[nvol-1];
		while ((idx >= 1) && (k[idx].val > mxNoise)) {
			tp ++;
			idx --;
			if ((k[idx].val != k[idx-1].val) && (k[idx].val <= threshold)  ) {
				fprintf(txt, "%g %g %g\n", (double)fp/(double)nvol, (double)tp/(double)nTrue, threshold);
				threshold = threshold - step; //delay next report
			}
		} //more significant than any noise...
		int fpThreshInt = round(fpThresh * nvol); //stop when number of false positives exceed this
		for (int i = nvol-1; i >= 1; i--) {
			fp ++; //false alarm
			while ((idx >= 1) && (k[idx].val >= mxVox[i])) {
				tp ++;
				idx --;
				if ((k[idx].val != k[idx-1].val) && (k[idx].val <= threshold)  ) {
					fprintf(txt, "%g %g %g\n", (double)fp/(double)nvol, (double)tp/(double)nTrue, threshold);
					threshold = threshold - step; //delay next report
				}
			} //at least as significant as current noise
			if ((fp > fpThreshInt) || ((k[i].val != k[i-1].val) && (k[i].val <= threshold)  ) ) {
				//niimath_print("%g %g %g\n", (double)fp/(double)nFalse, (double)tp/(double)nTrue, threshold);
				fprintf(txt, "%g %g %g\n", (double)fp/(double)nvol, (double)tp/(double)nTrue, threshold);
				threshold = threshold - step; //delay next report
			}
			if (fp > fpThreshInt) break;
		} //inspect all tests...
		_mm_free (mxVox);	
#ifdef USING_R
        return 1;
#else
		exit(1);
#endif
		
	} else { //if noise image else infer FP/TP from input image	
		int nFalse = nTest - nTrue;
		int fpThreshInt = ceil(fpThresh * nFalse); //stop when number of false positives exceed this
	
		for (int i = nTest-1; i >= 1; i--) {
			if (k[i].idx == 0) 
				fp ++; //false alarm
			else
				tp ++; //hit
			if ((fp > fpThreshInt) || ((k[i].val != k[i-1].val) && (k[i].val <= threshold)  ) ) {
				//niimath_print("%g %g %g\n", (double)fp/(double)nFalse, (double)tp/(double)nTrue, threshold);
				fprintf(txt, "%g %g %g\n", (double)fp/(double)nFalse, (double)tp/(double)nTrue, threshold);
				threshold = threshold - step; //delay next report
			}
			if (fp > fpThreshInt) break;
		} //inspect all tests...
	} //if noise else...
	fclose(txt);
	_mm_free (k);
	nifti_image_free( nimTrue );
	return 0;
}

static int nifti_fillh (nifti_image * nim, int is26)  { 
	if (nim->nvox < 1) return 1;
	if (nim->datatype != DT_CALC)
		return 1;
	int nvox3D = nim->dim[1] * nim->dim[2] * nim->dim[3];
	int nvol = nim->nvox / nvox3D;
	//size_t nxy = nim->nx * nim->ny; //slice increment
	uint8_t * vx = (uint8_t *)_mm_malloc(nim->nvox*sizeof(uint8_t), 64); 
	memset(vx, 0, nim->nvox*sizeof(uint8_t));
	size_t n1 = 0;
	flt * f32 = (flt *) nim->data;
	for (size_t i = 0; i < nim->nvox; i++ )
		if (f32[i] > 0.0) {
			n1++;
			vx[i] = 1;
		}
	if ((n1 < 1) || (nim->nx < 3) || (nim->ny < 3) || (nim->nz < 3)) { 
		//if fewer than 3 rows, columns or slices all voxels touch edge. 
		//only a binary threshold, not a flood fill
		for (size_t i = 0; i < nim->nvox; i++ )
			f32[i] = vx[i];
		_mm_free (vx); 
		return 1;
	}
	//set up kernel to search for neighbors. Since we already included sides, we do not worry about A<->P and L<->R wrap
	int numk = 6;
	if (is26) numk = 26;
	int32_t * k = (int32_t *)_mm_malloc(numk*sizeof(int32_t), 64); //queue with untested seed
	if (is26) {
		int j = 0;
		for (int z = -1; z <= 1; z++ )
			for (int y = -1; y <= 1; y++ )
				for (int x = -1; x <= 1; x++ ) {
					k[j] = x + (y * nim->nx) + (z * nim->nx * nim->ny);
					j++;
				} //for x
	} else { //if 26 neighbors else 6..
		k[0] = nim->nx * nim->ny; //up
		k[1] = -k[0]; //down
		k[2] = nim->nx; //anterior
		k[3] = -k[2]; //posterior
		k[4] = 1; //left
		k[5] = -1;
	}
	//https://en.wikipedia.org/wiki/Flood_fill	
	#pragma omp parallel for
	for (int v = 0; v < nvol; v++ ) {
		uint8_t * vxv = vx;
		vxv += (v * nvox3D);
		uint8_t * vxs = (uint8_t *)_mm_malloc(nim->nvox*sizeof(uint8_t), 64);
		memcpy(vxs, vxv, nvox3D*sizeof(uint8_t)); //dst, src
		int32_t * q = (int32_t *)_mm_malloc(nvox3D*sizeof(int32_t), 64); //queue with untested seed
		int qlo = 0;
        int qhi = -1; //ints always signed in C!
        //load edges
        size_t i = 0;
        for (int z = 0; z < nim->nz; z++ ) {
        	int zedge = 0;
        	if ((z == 0) || (z == (nim->nz-1))) zedge = 1;	
        	for (int y = 0; y < nim->ny; y++ ) {
        		int yedge = 0;
        		if ((y == 0) || (y == (nim->ny-1))) yedge = 1;	
        		for (int x = 0; x < nim->nx; x++ ) {
        			if ((vxs[i] == 0) && (zedge || yedge || (x == 0) || (x == (nim->nx-1))) ) { //found new seed
        				vxs[i] = 1; //do not find again
        				qhi++;
        				q[qhi] = i;	
        			} // new seed
        			i++;
        		} //for x
        	}//y
        } //z
		//niimath_print("seeds %d kernel %d\n", qhi+1, numk);
        //run a 'first in, first out' queue
        while (qhi >= qlo) {
        	//retire one seed, add 0..6 new ones (fillh) or 0..26 new ones (fillh26)
        	for (int j = 0; j < numk; j++) {
        		int jj = q[qlo] + k[j];
        		if ((jj < 0) || (jj >= nvox3D)) continue;
        		if (vxs[jj] != 0) continue;
        		//add new seed;
        		vxs[jj] = 1;
        		qhi++;
        		q[qhi] = jj;	 
        	}        	
        	qlo++;
        } //while qhi >= qlo: continue until all seeds tested
		for (size_t i = 0; i < nvox3D; i++ )
			if (vxs[i] == 0) vxv[i] = 1; //hidden internal voxel not found from the fill
		_mm_free (vxs);
		_mm_free (q);			
	} //for each volume		
	for (size_t i = 0; i < nim->nvox; i++ )
		f32[i] = vx[i];
	_mm_free (vx);
	_mm_free (k);
	return 0;
}

#ifndef USING_R
static void rand_test() {
	//https://www.phoronix.com/scan.php?page=news_item&px=Linux-RdRand-Sanity-Check
	int r0 = rand();
	for (int i = 0; i < 7; i++ )
		if (rand() != r0) return;
	niimath_message("RDRAND gives funky output: update firmware\n");
}
#endif

static int nifti_unary ( nifti_image * nim, enum eOp op) {
	if (nim->nvox < 1) return 1;
	if (nim->datatype != DT_CALC) {
		niimath_message("nifti_unary: Unsupported datatype %d\n", nim->datatype);
		return 1;
	}
	flt * f32 = (flt *) nim->data;
	if (op == exp1) {
		for (size_t i = 0; i < nim->nvox; i++ )
			f32[i] = exp(f32[i]);
	} else if (op == log1) {
		for (size_t i = 0; i < nim->nvox; i++ )
			f32[i] = log(f32[i]);
	} else if (op == sin1) {
		for (size_t i = 0; i < nim->nvox; i++ )
			f32[i] = sin(f32[i]);
	} else if (op == cos1) {
		for (size_t i = 0; i < nim->nvox; i++ )
			f32[i] = cos(f32[i]);
	} else if (op == tan1) {
		for (size_t i = 0; i < nim->nvox; i++ )
			f32[i] = tan(f32[i]);
	} else if (op == asin1) {
		for (size_t i = 0; i < nim->nvox; i++ )
			f32[i] = asin(f32[i]);
	} else if (op == acos1) {
		for (size_t i = 0; i < nim->nvox; i++ )
			f32[i] = acos(f32[i]);
	} else if (op == atan1) {
		for (size_t i = 0; i < nim->nvox; i++ )
			f32[i] = atan(f32[i]);
	} else if (op == sqr1) {
		for (size_t i = 0; i < nim->nvox; i++ )
			f32[i] = f32[i]*f32[i]; //<- pow(a,x) uses flt for x 
	} else if (op == sqrt1) {
		for (size_t i = 0; i < nim->nvox; i++ )
			f32[i] = sqrt(f32[i]);
	} else if (op == recip1) {
		for (size_t i = 0; i < nim->nvox; i++ ) {
			if (f32[i] == 0.0f) continue;
				f32[i] = 1.0 / f32[i];
		}
	} else if (op == abs1) {
		for (size_t i = 0; i < nim->nvox; i++ )
			f32[i] = fabs(f32[i]);
	} else if (op == bin1) {
		for (size_t i = 0; i < nim->nvox; i++ ) {
			if (f32[i] > 0) 
				f32[i] = 1.0f;
			else
				f32[i] = 0.0f;
		}		
	} else if (op == binv1) {
		for (size_t i = 0; i < nim->nvox; i++ ) {
			if (f32[i] > 0) 
				f32[i] = 0.0f;
			else
				f32[i] = 1.0f;
		}
	} else if (op == edge1) {
		if ((nim->dx == 0.0) || (nim->dy == 0.0) || (nim->dz == 0.0)) {
			niimath_message("edge requires non-zero pixdim1/pixdim2/pixdim3\n");
			return 1;		
		}
		flt xscl = 1.0/(sqr(nim->dx));
		flt yscl = 1.0/(sqr(nim->dy));
		flt zscl = 1.0/(sqr(nim->dz));
		flt xyzscl = 1.0/(2.0 * sqrt(xscl+yscl+zscl));
		if (nim->dim[3] < 2) { //no slices 'above' or 'below' for 2D
			size_t nxy = nim->nx * nim->ny; //slice increment
			int nvol = nim->nvox / nxy;
			if ((nvol * nxy) != nim->nvox) return 1;
			#pragma omp parallel for
			for (int v = 0; v < nvol; v++ ) { //find maximum for each entire volume (excepted observed volume 0)
				flt * inp = (flt *)_mm_malloc(nxy*sizeof(flt), 64);
				flt *o32 = (flt *) f32;
				o32 += v * nxy;
				memcpy(inp, o32, nxy*sizeof(flt)); //dst, src
				for (int y = 1; (y < (nim->ny -1)); y++ ) {
					size_t yo =y * nim->nx;
					for (int x = 1; (x < (nim->nx -1)); x++ ) {
						size_t vx = yo + x;
						flt xv = sqr(inp[vx+1] - inp[vx-1]) * xscl;
						flt yv = sqr(inp[vx+nim->nx] - inp[vx-nim->nx]) * yscl;
						o32[vx] = sqrt(xv+yv)*xyzscl;
					} //x
				} //y
				_mm_free (inp);
			}//for v
			return 1;
		} //edge for 2D volume(s)
		int nvox3D = nim->dim[1] * nim->dim[2] * nim->dim[3];
		int nvol = nim->nvox / nvox3D;
		if ((nvox3D * nvol) != nim->nvox) return 1;
		size_t nxy = nim->nx * nim->ny; //slice increment
		#pragma omp parallel for
		for (int v = 0; v < nvol; v++ ) { //find maximum for each entire volume (excepted observed volume 0)
			flt * inp = (flt *)_mm_malloc(nvox3D*sizeof(flt), 64);
			flt *o32 = (flt *) f32;
			o32 += v * nvox3D;
			memcpy(inp, o32, nvox3D*sizeof(flt)); //dst, src
			for (int z = 1; (z < (nim->nz -1)); z++ ) {
				size_t zo = z * nxy;
				for (int y = 1; (y < (nim->ny -1)); y++ ) {
					size_t yo =y * nim->nx;
					for (int x = 1; (x < (nim->nx -1)); x++ ) {
						size_t vx = zo + yo + x;
						flt xv = sqr(inp[vx+1] - inp[vx-1]) * xscl;
						flt yv = sqr(inp[vx+nim->nx] - inp[vx-nim->nx]) * yscl;
						flt zv = sqr(inp[vx+nxy] - inp[vx-nxy]) * zscl;
						o32[vx] = sqrt(xv+yv+zv)*xyzscl;
					} //x
				} //y
			} //z
			_mm_free (inp);
		}//for v
		return 1; //edge for 3D volume(s)	
	} else if (op == index1) {
		//nb FSLmaths flips dim[1] depending on determinant
		size_t idx = 0;
		if (!neg_determ(nim)) { //flip x
			size_t nyzt = nim->nvox / nim->nx;
			if ((nyzt * nim->nx) != nim->nvox) return 1;
			for (size_t i = 0; i <nyzt; i++ ) {
				size_t row = i * nim->nx;;
				int x = nim->nx;
				while (x > 0) {
					x--;
					if (f32[row+x] != 0) 
						f32[row+x] = idx++;	
				} //for each column (x)
			} //for each row (yzt)
		} else //don't flip x
			for (size_t i = 0; i < nim->nvox; i++ )
				if (f32[i] != 0) 
					f32[i] = idx++;
	} else if (op == nan1) {
		for (size_t i = 0; i < nim->nvox; i++ )
			if (isnan(f32[i]))
				f32[i] = 0.0;
	} else if (op == nanm1) {
		for (size_t i = 0; i < nim->nvox; i++ )
			if (isnan(f32[i]))
				f32[i] = 1.0;
			else
				f32[i] = 0.0;
	} else if (op == rand1) {
#ifdef USING_R
        for (size_t i = 0; i < nim->nvox; i++ )
            f32[i] += (flt) unif_rand();
#else
		rand_test();
		flt scl = (1.0 / RAND_MAX);
		for (size_t i = 0; i < nim->nvox; i++ )
			f32[i] += rand() * scl;	
#endif
	} else if (op == randn1) {
#ifdef USING_R
        for (size_t i = 0; i < nim->nvox; i++ )
            f32[i] += (flt) norm_rand();
#else
		rand_test();
		//https://en.wikipedia.org/wiki/Box–Muller_transform
		//for SIMD see https://github.com/miloyip/normaldist-benchmark
		static const flt sigma = 1.0f;
		static const flt mu = 0.0;
		//static const flt epsilon = FLT_EPSILON;
		static const flt two_pi = 2.0*3.14159265358979323846;
		static const flt scl = (1.0 / RAND_MAX);
		//fill pairs
		for (size_t i = 0; i < (nim->nvox-1); i += 2 ) {
			flt u1, u2;
			do {
				u1 = rand() * scl;
				u2 = rand() * scl;
			}
			while (u1 <= epsilon);
			flt su1 = sqrt(-2.0 * log(u1)); 
			flt z0 = su1 * cos(two_pi * u2);
			flt z1 = su1 * sin(two_pi * u2);
			f32[i] += z0 * sigma + mu;
			f32[i+1] += z1 * sigma + mu;
		}
		//if odd, fill final voxel
		if ( nim->nvox %2 != 0 ) {
			flt u1, u2;
			do {
				u1 = rand() * scl;
				u2 = rand() * scl;
			}
			while (u1 <= epsilon);
			flt z0 = sqrt(-2.0 * log(u1)) * cos(two_pi * u2);
			f32[nim->nvox-1] += z0 * sigma + mu;	
		}
#endif
	} else if (op == range1) { 			
		flt mn = f32[0];
		flt mx = mn;
		for (size_t i = 0; i < nim->nvox; i++ ) {
			mn = fmin(f32[i], mn);
			mx = fmax(f32[i],mx);
		} 
		nim->cal_min = mn;
		nim->cal_max = mx;
	} else if (op == rank1) {
		int nvox3D = nim->dim[1] * nim->dim[2] * nim->dim[3];
		int nvol = nim->nvox / nvox3D;
		if ((nvox3D * nvol) != nim->nvox) return 1;
		if (nvol <= 1 ) {
			//you are always first if you are the only one to show up...		
			for (size_t i = 0; i < nim->nvox; i++ )
				f32[i] = 1;	
		} else { 			
			#pragma omp parallel for
			for (int i = 0; i < nvox3D; i++ ) {
				//how do we handle ties?
				struct sortIdx * k = (struct sortIdx *)_mm_malloc(nvol*sizeof(struct sortIdx), 64); 
				size_t j = i;
				for (int v = 0; v < nvol; v++ ) { 
					k[v].val = f32[j];
					k[v].idx = j;
					j += nvox3D;
				}
				int varies = 0;
				for (int v = 0; v < nvol; v++ ) { 
					if (k[v].val != k[0].val) {
						varies = 1;
						break;
					}
				}
				if (varies) { 
					qsort (k, nvol, sizeof(struct sortIdx), compare);
					for (int v = 0; v < nvol; v++ )
						f32[ k[v].idx ] = v + 1;
				} else {
					j = i;
					for (int v = 0; v < nvol; v++ ) {
						f32[j] = v + 1;
						j += nvox3D;
					}					
				}
				_mm_free (k);	
			} //for i
		} //nvol > 1
	} else if ((op == rank1) || (op == ranknorm1)) {
		int nvox3D = nim->dim[1] * nim->dim[2] * nim->dim[3];
		int nvol = nim->nvox / nvox3D;
		if ((nvox3D * nvol) != nim->nvox) return 1;
		if (nvol <= 1 ) {
			//you are always first if you are the only one to show up...		
			for (int i = 0; i < nim->nvox; i++ )
				f32[i] = 0;	
		} else { 			
			#pragma omp parallel for
			for (int i = 0; i < nvox3D; i++ ) {
				struct sortIdx * k = (struct sortIdx *)_mm_malloc(nvol*sizeof(struct sortIdx), 64); 
				size_t j = i;
				double sum = 0.0;
				for (int v = 0; v < nvol; v++ ) { 
					k[v].val = f32[j];
					sum += k[v].val;
					k[v].idx = j;
					j += nvox3D;
				}
				double mean = sum / nvol;
				double sumSqr = 0.0;
				for (int v = 0; v < nvol; v++ )
					sumSqr += sqr(k[v].val- mean);
				double stdev = sqrt(sumSqr / (nvol - 1));
				
				qsort (k, nvol, sizeof(struct sortIdx), compare);
				//strange formula, but replicates fslmaths, consider nvol=3 rank[2,0,1] will be pval [2.5/3, 1.5/3, 0.5/3]
				for (int v = 0; v < nvol; v++ )
					f32[ k[v].idx ] = (stdev * -qginv((double)(v + 0.5)/(double)nvol)) + mean;
				_mm_free (k);	
			} //for i
		} //nvol > 1	
	//double qginv( double p )
	} else if (op == ztop1) { 			
		for (size_t i = 0; i < nim->nvox; i++ )
			f32[i] = qg(f32[i]);
	} else if (op == ptoz1) { 			
		/*** given p, return x such that Q(x)=p, for 0 < p < 1 ***/
		// #ifdef DT32
		const flt kNaN = NAN;
		//const flt kNaN = 0.0 / 0.0;
		for (size_t i = 0; i < nim->nvox; i++ ) {
			if ((f32[i] < 0.0) || (f32[i] > 1.0))
				f32[i] = kNaN;
			else
				f32[i] = qginv(f32[i]);
		}
	} else if ((op == pval1) || (op == pval01)) {
		int nvox3D = nim->dim[1] * nim->dim[2] * nim->dim[3];
		int nvol = nim->nvox / nvox3D;
		if ((nvox3D * nvol) != nim->nvox) return 1;
		if (nvol <= 1 ) {
			niimath_message("permutation tests require 4D datasets.\n");
			return 1; 		
		}
		void * dat = (void *)calloc(1, nvox3D * sizeof(flt)) ;
		flt *o32 = (flt *) dat;
		#pragma omp parallel for
		for (int i = 0; i < nvox3D; i++ ) {
			size_t vi = i;
			flt obs = f32[vi]; //observed value - see if it is extreme relative to permutations
			int nNotZero = 0;
			int nGreater = 0;
			int nEqual = 0; //observation in first volume
			flt f32v0 = f32[vi];
			for (int v = 0; v < nvol; v++ ) { 
				if (f32[vi] != 0) nNotZero ++;
				if (f32[vi] == f32v0) nEqual ++;
				if (f32[vi] >= obs) nGreater ++;	
				vi += nvox3D;
			}
			if (op == pval1) {
				//if (nEqual == nvol) 
				//	o32[i] = 0.0;
				//else
					o32[i] = (double)nGreater / (double) nvol ;	
			} else {
				if (nEqual == nvol) 
					o32[i] = 0.0;
				else if (obs == 0)
					o32[i] = 1.0;
				else //nZero must be at least 1: the observed data is not zero
					o32[i] = (double)nGreater / (double) (nNotZero) ;	
			}
		} //for i
		nim->nvox = nvox3D;
		nim->ndim = 3;
		nim->nt = 1;
		nim->dim[0] = 3;
		nim->dim[4] = 1;
		free(nim->data);
		nim->data = dat; 
	} else if (op == cpval1) {
		int nvox3D = nim->dim[1] * nim->dim[2] * nim->dim[3];
		int nvol = nim->nvox / nvox3D;
		if ((nvox3D * nvol) != nim->nvox) return 1;
		if (nvol <= 1 ) {
			niimath_message("permutation tests require 4D datasets.\n");
			return 1; 		
		}
		void * dat = (void *)calloc(1, nvox3D * sizeof(flt)) ;
		flt *o32 = (flt *) dat;
		flt * vmax = (flt *)_mm_malloc(nvol*sizeof(flt), 64); 
		#pragma omp parallel for
		for (int v = 1; v < nvol; v++ ) { //find maximum for each entire volume (excepted observed volume 0)
			size_t vo = v * nvox3D;
			flt mx = f32[vo];
			for (int i = 0; i < nvox3D; i++ )
				mx = MAX(mx, f32[vo+i]);
			vmax[v] = mx;
			//niimath_print("%d %g\n", v, mx);
		}
		#pragma omp parallel for
		for (int i = 0; i < nvox3D; i++ ) {
			flt obs = f32[i]; //observed value - see if it is extreme relative to permutations
			int nGreater = 1; //count observation
			for (int v = 1; v < nvol; v++ ) 
				if (vmax[v] >= obs) nGreater ++;	
			o32[i] = (double)nGreater / (double) nvol ;	
		} //for i
		_mm_free (vmax);		
		nim->nvox = nvox3D;
		nim->ndim = 3;
		nim->nt = 1;
		nim->dim[0] = 3;
		nim->dim[4] = 1;
		free(nim->data);
		nim->data = dat; 	
	} else {
		niimath_message("nifti_unary: Unsupported operation\n");
		return 1;	
	}
	return 0;
}//nifti_unary()

static int nifti_thrp(nifti_image * nim, double v, enum eOp op) {
// -thrp: use following percentage (0-100) of ROBUST RANGE to threshold current image (zero anything below the number)
// -thrP: use following percentage (0-100) of ROBUST RANGE of non-zero voxels and threshold below
// -uthrp : use following percentage (0-100) of ROBUST RANGE to upper-threshold current image (zero anything above the number)
// -uthrP : use following percentage (0-100) of ROBUST RANGE of non-zero voxels and threshold above
	if ((v <= 0.0) || (v >= 100.0)) {
		niimath_message("nifti_thrp: threshold should be between 0..100\n");
		return 1;
	}
	flt pct2, pct98;
	int ignoreZeroVoxels = 0;
	if ((op == thrP) || (op == uthrP))
		ignoreZeroVoxels = 1;	
	if (nifti_robust_range(nim, &pct2, &pct98, ignoreZeroVoxels) != 0)
		return 1;
	flt thresh = pct2 + ((v/100.0) * (pct98-pct2));
	int zeroBrightVoxels = 0;
	if ((op == uthrp) || (op == uthrP))
		zeroBrightVoxels = 1;	
	nifti_thr(nim, thresh, zeroBrightVoxels);	
	return 0;
} //nifti_thrp()
 
#ifdef DT32
int main32(int argc, char * argv[]) {
#else
int main64(int argc, char * argv[]) {
#ifndef USING_R
	niimath_print("beta: Using 64-bit calc\n");
#endif
#endif
	char * fin=NULL, * fout=NULL;
	//fslmaths in.nii out.nii changes datatype to flt, here we retain (similar to earlier versions of fslmaths)
	//fslmsths in.nii -rem 10 out.nii uses integer modulus not fmod
	//fslmaths robust range not fully described, this emulation is close
	//fslmaths ing/inm are listed as "unary" but should be listed as binary
	if( argc < 3 ) return show_helpx(); //minimal command has input and output: "niimath in.nii out.nii"
	int dtCalc = DT_FLOAT32; //data type for calculation
	int dtOut = DT_FLOAT32; //data type for calculation
	int ac = 1;
	// '-dt' sets datatype for calculations
	if( ! strcmp(argv[ac], "-dt") ) {
		if (! strcmp(argv[ac+1], "double") ) {
			dtCalc = DT_FLOAT64;
		} else if (strcmp(argv[ac+1], "float") ) {
			niimath_message("'-dt' error: only float or double calculations supported\n");
			return 1;
		}
		ac += 2;
		if( argc < (ac+2) ) return 1; //insufficient arguments remain
	}
	//special case: pass through
	// no calculation, simple pass through copy, e.g. "niimaths in.nii out.nii.gz"
	// note fslmaths would save as flt type... but lossless conversion in native format is faster
	// note here we use nifti_image_read not nifti_image_read2 to preserve cal_min, cal_max
#ifndef USING_R
	if (ac+2 == argc) {
		fin = argv[ac]; /* no string copy, just pointer assignment */
		ac ++;
		nifti_image * nim = nifti_image_read(fin, 1);
		fout = argv[ac]; /* no string copy, just pointer assignment */
		ac ++;
		if (nifti_set_filenames(nim, fout, 0, 1) ) return 1;
		nifti_save(nim, ""); //nifti_image_write( nim );
		nifti_image_free( nim );
		return 0;
	} //end pass through
#endif
	// next argument is input file
	fin = argv[ac]; /* no string copy, just pointer assignment */
	ac ++;
	//clock_t startTime = clock();
	nifti_image * nim = nifti_image_read2(fin, 1);
	if( !nim ) {
		niimath_message("** failed to read NIfTI image from '%s'\n", fin);
		return 2;
	}
	//niimath_print("read time: %ld ms\n", timediff(startTime, clock()));
	in_hdr ihdr = set_input_hdr(nim);
	int nkernel = 0; //number of voxels in kernel
	int * kernel = make_kernel(nim, &nkernel, 3,3,3);
	//check for "-odt" must be last couplet 
	if ( ! strcmp(argv[argc-2], "-odt") ) {
		if (! strcmp(argv[argc-1], "double") ) {
			dtOut = DT_FLOAT64;
		} else if (! strcmp(argv[argc-1], "flt") ) {
			dtOut = DT_FLOAT32;
		} else if (! strcmp(argv[argc-1], "int") ) {
			dtOut = DT_INT32;
		} else if (! strcmp(argv[argc-1], "short") ) {
			dtOut = DT_INT16;
		} else if (! strcmp(argv[argc-1], "ushort") ) {
			dtOut = DT_UINT16;
		} else if (! strcmp(argv[argc-1], "char") ) {
			dtOut = DT_UINT8;
		} else if (! strcmp(argv[argc-1], "input") ) {
			dtOut = nim->datatype;//ihdr.datatype; //!
		} else {
			niimath_message("Error: Unknown datatype '%s' - Possible datatypes are: char short ushort int flt double input\n", argv[argc-1]);
			return 2;
		}
		argc = argc - 2;
	} //odt
	//convert data to calculation type (-dt)
	if (nifti_image_change_datatype(nim, dtCalc, &ihdr) != 0) return 1; 
	//check output filename, e.g does file exist
#ifndef USING_R
	fout = argv[argc-1]; /* no string copy, just pointer assignment */
	if( nifti_set_filenames(nim, fout, 0, 1) ) return 1;
	argc = argc - 1;
#endif
	#if defined(_OPENMP) 
		const int maxNumThreads = omp_get_max_threads();
		const char *key = "AFNI_COMPRESSOR";
		char *value;
      	value = getenv(key);
      	//export AFNI_COMPRESSOR=PIGZ
      	char pigzKey[5] = "PIGZ";
      	if ((value != NULL) && (strstr(value,pigzKey))) {
      		omp_set_num_threads(maxNumThreads);
      		niimath_message("Using %d threads\n", maxNumThreads);
      	} else {
      		omp_set_num_threads(1);
      		niimath_message("Single threaded\n");
      	}	
	#endif
	//read operations
	char* end;
	int ok = 0;
	while (ac < argc) {
		enum eOp op = unknown;
		if ( ! strcmp(argv[ac], "-add") ) op = add;
		if ( ! strcmp(argv[ac], "-sub") ) op = sub;
		if ( ! strcmp(argv[ac], "-mul") ) op = mul;
		if ( ! strcmp(argv[ac], "-div") ) op = divX;
		if ( ! strcmp(argv[ac], "-rem") ) op = rem;
		if ( ! strcmp(argv[ac], "-mod") ) op = mod;
		if ( ! strcmp(argv[ac], "-mas") ) op = mas;
		if ( ! strcmp(argv[ac], "-thr") ) op = thr;
		if ( ! strcmp(argv[ac], "-thrp") ) op = thrp;
		if ( ! strcmp(argv[ac], "-thrP") ) op = thrP;
		if ( ! strcmp(argv[ac], "-uthr") ) op = uthr;
		if ( ! strcmp(argv[ac], "-uthrp") ) op = uthrp;
		if ( ! strcmp(argv[ac], "-uthrP") ) op = uthrP;
		if ( ! strcmp(argv[ac], "-max") ) op = max;
		if ( ! strcmp(argv[ac], "-min") ) op = min;
		if ( ! strcmp(argv[ac], "-max") ) op = max;
		//if ( ! strcmp(argv[ac], "-addtozero") ) op = addtozero; //variation of mas
		//if ( ! strcmp(argv[ac], "-overadd") ) op = overadd; //variation of mas
		if ( ! strcmp(argv[ac], "power") ) op = power;
		if ( ! strcmp(argv[ac], "-seed") ) op = seed;
		//if ( ! strcmp(argv[ac], "-restart") ) op = restart;
		//if ( ! strcmp(argv[ac], "-save") ) op = save;		
		if ( ! strcmp(argv[ac], "-inm") ) op = inm;
		if ( ! strcmp(argv[ac], "-ing") ) op = ing;
		if ( ! strcmp(argv[ac], "-s") ) op = smth;
		if ( ! strcmp(argv[ac], "-exp") ) op = exp1;
		if ( ! strcmp(argv[ac], "-log") ) op = log1;
		if ( ! strcmp(argv[ac], "-sin") ) op = sin1;
		if ( ! strcmp(argv[ac], "-cos") ) op = cos1;
		if ( ! strcmp(argv[ac], "-tan") ) op = tan1;
		if ( ! strcmp(argv[ac], "-asin") ) op = asin1;
		if ( ! strcmp(argv[ac], "-acos") ) op = acos1;
		if ( ! strcmp(argv[ac], "-atan") ) op = atan1;
		if ( ! strcmp(argv[ac], "-sqr") ) op = sqr1;
		if ( ! strcmp(argv[ac], "-sqrt") ) op = sqrt1;
		if ( ! strcmp(argv[ac], "-recip") ) op = recip1;
		if ( ! strcmp(argv[ac], "-abs") ) op = abs1;
		if ( ! strcmp(argv[ac], "-bin") ) op = bin1;
		if ( ! strcmp(argv[ac], "-binv") ) op = binv1;
		if ( ! strcmp(argv[ac], "-edge") ) op = edge1;
		if ( ! strcmp(argv[ac], "-index") ) op = index1;
		if ( ! strcmp(argv[ac], "-nan") ) op = nan1;
		if ( ! strcmp(argv[ac], "-nanm") ) op = nanm1;
		if ( ! strcmp(argv[ac], "-rand") ) op = rand1;
		if ( ! strcmp(argv[ac], "-randn") ) op = randn1;
		if ( ! strcmp(argv[ac], "-range") ) op = range1;
		if ( ! strcmp(argv[ac], "-rank") ) op = rank1;
		if ( ! strcmp(argv[ac], "-ranknorm") ) op = ranknorm1;
		if ( ! strcmp(argv[ac], "-ztop") ) op = ztop1;
		if ( ! strcmp(argv[ac], "-ptoz") ) op = ptoz1;
		if ( ! strcmp(argv[ac], "-pval") ) op = pval1;
		if ( ! strcmp(argv[ac], "-pval0") ) op = pval01;
		if ( ! strcmp(argv[ac], "-cpval") ) op = cpval1;
		//kernel operations
		if ( ! strcmp(argv[ac], "-dilM") ) op = dilMk;
		if ( ! strcmp(argv[ac], "-dilD") ) op = dilDk;
		if ( ! strcmp(argv[ac], "-dilF") ) op = dilFk;
		if ( ! strcmp(argv[ac], "-dilall") ) op = dilallk;
		if ( ! strcmp(argv[ac], "-ero") ) op = erok;
		if ( ! strcmp(argv[ac], "-eroF") ) op = eroFk;
		if ( ! strcmp(argv[ac], "-fmedian") ) op = fmediank;
		if ( ! strcmp(argv[ac], "-fmean") ) op = fmeank;
		if ( ! strcmp(argv[ac], "-fmeanu") ) op = fmeanuk;
		if ( ! strcmp(argv[ac], "-p") ) {
			ac++;
			#if defined(_OPENMP) 
			int nProcessors = atoi(argv[ac]);
			if (nProcessors < 1) { 
				omp_set_num_threads(maxNumThreads);
				niimath_message("Using %d threads\n", maxNumThreads);
			} else
				omp_set_num_threads(nProcessors);
			
				
			#else
			niimath_message("Warning: not compiled for OpenMP: '-p' ignored\n");
			#endif		
		} else
		//All Dimensionality reduction operations names begin with Capital letter, no other commands do!
		if ((strlen(argv[ac]) > 4) && (argv[ac][0] == '-') && (isupper(argv[ac][1]))) { //isupper
			int dim = 0;
			switch (argv[ac][1]) {
				case 'X': //
					dim = 1;
					break;
				case 'Y': // code to be executed if n = 2;
					dim = 2;
					break;
				case 'Z': //
					dim = 3;
					break;
				case 'T': // code to be executed if n = 2;
					dim = 4;
					break;
			}
			if (dim == 0) {
				niimath_message("Error: unknown dimensionality reduction operation: %s\n", argv[ac]);
				goto fail;
			}
			if ( strstr(argv[ac], "mean") ) 
				ok = nifti_dim_reduce(nim, Tmean, dim, 0);
			else if ( strstr(argv[ac], "std") ) 
				ok = nifti_dim_reduce(nim, Tstd, dim, 0);
			else if ( strstr(argv[ac], "maxn") ) 
				ok = nifti_dim_reduce(nim, Tmaxn, dim, 0); //test maxn BEFORE max
			else if ( strstr(argv[ac], "max") ) 
				ok = nifti_dim_reduce(nim, Tmax, dim, 0);
			else if ( strstr(argv[ac], "min") ) 
				ok = nifti_dim_reduce(nim, Tmin, dim, 0 );
			else if ( strstr(argv[ac], "median") ) 
				ok = nifti_dim_reduce(nim, Tmedian, dim, 0);
			else if ( strstr(argv[ac], "perc") ) {
				ac++;
				int pct = atoi(argv[ac]);
				ok = nifti_dim_reduce(nim, Tperc, dim, pct);
			} else if ( strstr(argv[ac], "ar1") ) 
				ok = nifti_dim_reduce(nim, Tar1, dim, 0);
			else {
				niimath_message("Error unknown dimensionality reduction operation: %s\n", argv[ac]);
				ok = 1;
			}
		} else if ( ! strcmp(argv[ac], "-roi") ) {
			//int , int , int , int , int , int , int , int )
			if ((argc-ac) < 8) {
				niimath_message("not enough arguments for '-roi'\n"); //start.size for 4 dimensions: user might forget volumes
				goto fail;
			}
			ac ++;
			int xmin = atoi(argv[ac]);
			ac ++;
			int xsize = atoi(argv[ac]);
			ac ++;
			int ymin = atoi(argv[ac]);
			ac ++;
			int ysize = atoi(argv[ac]);
			ac ++;
			int zmin = atoi(argv[ac]);
			ac ++;
			int zsize = atoi(argv[ac]);
			ac ++;
			int tmin = atoi(argv[ac]);
			ac ++;
			int tsize = atoi(argv[ac]);
			nifti_roi(nim, xmin, xsize, ymin, ysize, zmin, zsize, tmin, tsize);
		} else if ( ! strcmp(argv[ac], "-bptfm") ) {
			ac++;
		 	double hp_sigma = strtod(argv[ac], &end);
		 	ac++;
		 	double lp_sigma = strtod(argv[ac], &end);
			ok = nifti_bptf(nim, hp_sigma, lp_sigma, 0);
		} else if ( ! strcmp(argv[ac], "-bptf") ) {
			ac++;
		 	double hp_sigma = strtod(argv[ac], &end);
		 	ac++;
		 	double lp_sigma = strtod(argv[ac], &end);
			//ok = nifti_bptf(nim, hp_sigma, lp_sigma);
			ok = nifti_bptf(nim, hp_sigma, lp_sigma, 1);
		#ifdef bandpass
		} else if ( ! strcmp(argv[ac], "-bandpass") ) {
			// niimath test4D -bandpass 0.08 0.008 0 c
			ac++;
		 	double lp_hz = strtod(argv[ac], &end);
		 	ac++;
		 	double hp_hz = strtod(argv[ac], &end);
		 	ac++;
		 	double TRsec = strtod(argv[ac], &end);
		 	ok = nifti_bandpass(nim, lp_hz, hp_hz, TRsec);
		#endif 
		} else if ( ! strcmp(argv[ac], "-roc") ) {
			//-roc <AROC-thresh> <outfile> [4Dnoiseonly] <truth>
			//-roc <AROC-thresh> <outfile> [4Dnoiseonly] <truth>
			ac++;
		 	double thresh = strtod(argv[ac], &end);
		 	ac++;
		 	int outfile = ac;
		 	char * fnoise =NULL;
		 	if (thresh > 0.0) {
		 		ac++;
		 		fnoise = argv[ac];
		 	}
		 	ac++;
		 	int truth = ac;
		 	//ok = nifti_bptf(nim, hp_sigma, lp_sigma);
			ok = nifti_roc(nim, fabs(thresh), argv[outfile], fnoise, argv[truth]);
			if (ac >= argc) {
				niimath_message("Error: no output filename specified!\n"); //e.g. volume size might differ
				goto fail;
			}
			  
		} else if ( ! strcmp(argv[ac], "-unsharp") ) {
			ac++;
		 	double sigma = strtod(argv[ac], &end);
		 	ac++;
		 	double amount = strtod(argv[ac], &end);
			nifti_unsharp(nim, sigma, sigma, sigma, amount);
		} else if ( ! strcmp(argv[ac], "-otsu") ) 
			ok = nifti_otsu(nim, 0);
		else if ( ! strcmp(argv[ac], "-otsu0") ) 
			ok = nifti_otsu(nim, 1);
		else if ( ! strcmp(argv[ac], "-subsamp2") ) 
			ok = nifti_subsamp2(nim, 0);
		else if ( ! strcmp(argv[ac], "-subsamp2offc") ) 
			ok = nifti_subsamp2(nim, 1);
		else if ( ! strcmp(argv[ac], "-sobel") ) 
			ok = nifti_sobel(nim, 1);
		else if ( ! strcmp(argv[ac], "-demean") ) 
			ok = nifti_demean(nim);
		else if ( ! strcmp(argv[ac], "-detrend") ) 
			ok = nifti_detrend_linear(nim);
		else if ( ! strcmp(argv[ac], "-resize") ) {
			ac++;
		 	double X = strtod(argv[ac], &end);
		 	ac++;
		 	double Y = strtod(argv[ac], &end);
		 	ac++;
		 	double Z = strtod(argv[ac], &end);
		 	ac ++;
			int interp_method = atoi(argv[ac]);
			ok = nifti_resize(nim, X, Y, Z, interp_method);
		} else if ( ! strcmp(argv[ac], "-crop") ) {
			ac ++;
			int tmin = atoi(argv[ac]);
			ac ++;
			int tsize = atoi(argv[ac]);
			ok = nifti_crop(nim, tmin, tsize);
#ifndef USING_R
		} else if ( ! strcmp(argv[ac], "--compare") ) { //--function terminates without saving image
			ac ++;
			nifti_compare(nim, argv[ac]); //always terminates
#endif
		} else if ( ! strcmp(argv[ac], "-edt") ) 
			ok = nifti_edt(nim);
		else if ( ! strcmp(argv[ac], "-fillh") ) 
			ok = nifti_fillh(nim, 0);
		else if ( ! strcmp(argv[ac], "-fillh26") ) 
			ok = nifti_fillh(nim, 1);
		else if ( ! strcmp(argv[ac], "-kernel") ) {
			ac ++;
			if (kernel != NULL) _mm_free(kernel);
			kernel = NULL;
			if ( ! strcmp(argv[ac], "3D") )
		 		kernel = make_kernel(nim, &nkernel, 3,3,3);
			if ( ! strcmp(argv[ac], "2D") )
		 		kernel = make_kernel(nim, &nkernel, 3,3,1);
		 	if ( ! strcmp(argv[ac], "boxv") ) {
		 		ac++;
		 		int vx = atoi(argv[ac]);
		 		kernel = make_kernel(nim, &nkernel, vx,vx,vx);
		 	}
		 	if ( ! strcmp(argv[ac], "sphere") ) {
		 		ac++;
		 		double mm = strtod(argv[ac], &end);
		 		kernel = make_kernel_sphere(nim, &nkernel, mm);
		 	}
		 	if ( ! strcmp(argv[ac], "file") ) {
		 		ac++;
		 		kernel = make_kernel_file(nim, &nkernel, argv[ac]);
		 	}
		 	if ( ! strcmp(argv[ac], "gauss") ) {
		 		ac++;
		 		double mm = strtod(argv[ac], &end);
		 		kernel = make_kernel_gauss(nim, &nkernel, mm);
		 	}
		 	if ( ! strcmp(argv[ac], "box") ) { //all voxels in a cube of width <size> mm centered on target voxel");
		 		ac++;
		 		double mm = strtod(argv[ac], &end);
		 		int vx = (2*floor(mm/nim->dx))+1;
				int vy = (2*floor(mm/nim->dy))+1;
				int vz = (2*floor(mm/nim->dz))+1;
		 		kernel = make_kernel(nim, &nkernel, vx,vy,vz);
		 	}
		 	if ( ! strcmp(argv[ac], "boxv3") ) {
		 		ac++;
		 		int vx = atoi(argv[ac]);
		 		ac++;
		 		int vy = atoi(argv[ac]);
		 		ac++;
		 		int vz = atoi(argv[ac]);
		 		kernel = make_kernel(nim, &nkernel, vx,vy,vz);
		 	}
		 	if (kernel == NULL){
		 		niimath_message("Error: '-kernel' option failed.\n"); //e.g. volume size might differ
				ok = 1; 
		 	}
		} else if ( ! strcmp(argv[ac], "-tensor_2lower") ) {
			ok = nifti_tensor_2(nim, 0);
		} else if ( ! strcmp(argv[ac], "-tensor_2upper") ) {
			ok = nifti_tensor_2(nim, 1);
		} else if ( ! strcmp(argv[ac], "-tensor_decomp") ) {
			ok = nifti_tensor_decomp(nim,1);
		} else if ( ! strcmp(argv[ac], "-tensor_decomp_lower") ) {
			ok = nifti_tensor_decomp(nim,0);
		} else if ( ! strcmp(argv[ac], "-slicetimer") ) {
			#ifdef slicetimer
			ok = nifti_slicetimer(nim);
			#else
			niimath_message("Recompile to support slice timer\n"); //e.g. volume size might differ
			ok = 1;
			#endif
		} else if ( ! strcmp(argv[ac], "-save") ) {
			ac ++;
			char * fout2 = argv[ac]; 
			if( nifti_set_filenames(nim, fout2, 1, 1) )
				ok = 1;
			else {
				nifti_save(nim, ""); //nifti_image_write( nim );
				nifti_set_filenames(nim, fout, 1, 1);
			}	
		} else if ( ! strcmp(argv[ac], "-restart") ) {
			if (kernel != NULL) 
				niimath_message("Warning: 'restart' resets the kernel\n"); //e.g. volume size might differ
			nifti_image_free( nim );
			if (kernel != NULL) _mm_free(kernel);
			kernel = make_kernel(nim, &nkernel, 3,3,3);
			ac++;
			nim = nifti_image_read(argv[ac], 1);
			if( !nim )ok = 1; //error
		} else if ( ! strcmp(argv[ac], "-grid") ) {
			ac++;
			double v = strtod(argv[ac], &end);
			ac++;
			int s = atoi(argv[ac]);
			ok = nifti_grid(nim, v, s);
		} else if ( ! strcmp(argv[ac], "-tfce") ) {
			ac++;
			double H = strtod(argv[ac], &end);
			ac++;
			double E = strtod(argv[ac], &end);
			ac++;
			int c = atoi(argv[ac]);
			ok = nifti_tfce(nim, H, E, c);
		} else if ( ! strcmp(argv[ac], "-tfceS") ) {
			ac++;
			double H = strtod(argv[ac], &end);
			ac++;
			double E = strtod(argv[ac], &end);
			ac++;
			int c = atoi(argv[ac]);
			ac++;
			int x = atoi(argv[ac]);
			ac++;
			int y = atoi(argv[ac]);
			ac++;
			int z = atoi(argv[ac]);
			ac++;
			double tfce_thresh = strtod(argv[ac], &end);
			ok = nifti_tfceS(nim, H, E, c, x, y, z, tfce_thresh);
		} else if (op == unknown) {
			niimath_message("!!Error: unsupported operation '%s'\n", argv[ac]);
			goto fail;
		}
		if ((op >= dilMk) && (op <= fmeanuk)) 
			ok = nifti_kernel (nim, op, kernel, nkernel);
		if ((op >= exp1) && (op <= ptoz1))
			 nifti_unary(nim, op);
		if ((op >= add) && (op < exp1)) { //binary operations
			ac++;
			double v = strtod(argv[ac], &end);
			//if (end == argv[ac]) {
			if (strlen(argv[ac]) != (end - argv[ac])) { // "4d" will return numeric "4"
				if ((op == power) || (op == thrp) || (op == thrP) || (op == uthrp) || (op == uthrP) || (op == seed) ) {
					niimath_message("Error: '%s' expects numeric value\n", argv[ac-1]);
					goto fail;
				} else
					ok = nifti_binary(nim, argv[ac], op);
			} else {
				if (op == add) 
					ok = nifti_rescale(nim , 1.0, v);
				if (op == sub) 
					ok = nifti_rescale(nim , 1.0, -v);
				if (op == mul) 
					ok = nifti_rescale(nim , v, 0.0);
				if (op == divX) 
					ok = nifti_rescale(nim , 1.0/v, 0.0);
				if (op == mod) 
					ok = nifti_rem(nim, v, 1);
				if (op == rem) 
					ok = nifti_rem(nim, v, 0);
				if (op == mas) { 
					niimath_message("Error: -mas expects image not number\n");
					goto fail;	
				}
				if (op == power)
					ok = nifti_binary_power(nim, v);
				if (op == thr) 
					ok = nifti_thr(nim, v, 0);
				if ((op == thrp) || (op == thrP) || (op == uthrp) || (op == uthrP))
					ok = nifti_thrp(nim, v, op);
				if (op == uthr) 
					ok = nifti_thr(nim, v, 1);
				if (op == max) 
					ok = nifti_max(nim, v, 0);
				if (op == min) 
					ok = nifti_max(nim, v, 1);
				if (op == inm)
					ok = nifti_inm(nim, v);
				if (op == ing)
					ok = nifti_ing(nim, v);
				if (op == smth)
					ok = nifti_smooth_gauss(nim, v, v, v);
#ifndef USING_R
				if (op == seed) {
					if ((v > 0) && (v < 1))
						v *= RAND_MAX;
					srand((unsigned)fabs(v));
				}
#endif
			}	
		} //binary operations
		if (ok != 0) goto fail;
		ac ++;
	}
	//convert data to output type (-odt)
	if (nifti_image_change_datatype(nim, dtOut, &ihdr) != 0) return 1;
	/* if we get here, write the output dataset */
	//startTime = clock();
	nifti_save(nim, ""); //nifti_image_write( nim );
	//niimath_print("write time: %ld ms\n", timediff(startTime, clock()));
	/* and clean up memory */
	nifti_image_free( nim );
	if (kernel != NULL) _mm_free(kernel);
	return 0;
	fail:
	nifti_image_free( nim );
	if (kernel != NULL) _mm_free(kernel);
	return 1; 
} //main()
