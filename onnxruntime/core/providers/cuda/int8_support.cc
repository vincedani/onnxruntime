//
// Copyright (c) 2017, NVIDIA CORPORATION. All rights reserved.
// Licensed under the MIT license. See LICENSE.md file in the project root for full license information.
//

// Make generic operators for floating point types
/* This file contains:
   Generalized library calls
   kernels to be called for not supported data type
*/
// NV_TODO: optimize speed -- pass things needed in, optimize kernel speed, add half2
// NV_TODO: investigate cub support for half

#include "core/providers/cuda/cuda_common.h"
#include "core/providers/cuda/shared_inc/cuda_call.h"

namespace onnxruntime {
namespace cuda {

static inline int roundoff(int v, int d) {
  return (v + d - 1) / d * d;
}

// Use cublasLtMatmul to perform the tensor op Igemm with the memory
// order transforms on all buffers.
//
// For better performance the data order transforms should be offline
// as much as possible.
//
// Transa, transb assumed N; alpha, beta are host pointers; Tensor ops
// allowed. Alpha assumed 1, beta assumed 0, and stream assumed 0.

void LtIgemmTensor(cublasLtHandle_t ltHandle,
                          int m,
                          int n,
                          int k,
                          int32_t alpha,
                          int32_t beta,
                          const int8_t* A,
                          int lda,
                          const int8_t* B,
                          int ldb,
                          int32_t* C,
                          int ldc,
                          const CudaKernel* cuda_kernel) {
  cublasLtMatmulDesc_t matmulDesc = NULL;
  cublasLtMatrixLayout_t a_desc = NULL;
  cublasLtMatrixLayout_t b_desc = NULL;
  cublasLtMatrixLayout_t c_desc = NULL;
  cublasOperation_t op_transpose = CUBLAS_OP_T;

  // The tensor op igemm kernels require specialized memory order of data
  cublasLtMatrixLayout_t AtransformDesc = NULL;
  cublasLtMatrixLayout_t BtransformDesc = NULL;
  cublasLtMatrixLayout_t CtransformDesc = NULL;
  float transformAlpha = 1.0f;
  float transformBeta = 0.0f;
  cublasLtOrder_t order_COL32 = CUBLASLT_ORDER_COL32;
  cublasLtOrder_t order_COL4_4R2_8C = CUBLASLT_ORDER_COL4_4R2_8C;

  int ldatransform = 32 * m;
  int ldbtransform = 32 * roundoff(n, 8);
  int ldctransform = 32 * m;

  IAllocatorUniquePtr<int8_t> a_transform = cuda_kernel->GetScratchBuffer<int8_t>(roundoff(k, 32) / 32 * ldatransform);
  IAllocatorUniquePtr<int8_t> b_transform = cuda_kernel->GetScratchBuffer<int8_t>(roundoff(k, 32) / 32 * ldbtransform);
  IAllocatorUniquePtr<int32_t> c_transform = cuda_kernel->GetScratchBuffer<int32_t>(roundoff(k, 32) / 32 * ldctransform);

  cublasLtMatrixTransformDesc_t transform_desc = NULL;
  CUBLAS_CALL_THROW(cublasLtMatrixTransformDescCreate(&transform_desc, CUDA_R_32F));

  //// B matrix is non-transposed, but transposed matrix is needed - add transpose operation in matrix transform.
  //CUBLAS_CALL_THROW(cublasLtMatrixTransformDescSetAttribute(transform_desc,
  //                                                          CUBLASLT_MATRIX_TRANSFORM_DESC_TRANSB,
  //                                                          &op_transpose,
  //                                                          sizeof(op_transpose)));

  // Tensor op igemm kernels only support NT gemm
  CUBLAS_CALL_THROW(cublasLtMatmulDescCreate(&matmulDesc, CUDA_R_32I));
  CUBLAS_CALL_THROW(cublasLtMatmulDescSetAttribute(matmulDesc, CUBLASLT_MATMUL_DESC_TRANSB, &op_transpose, sizeof(op_transpose)));

  // Create descriptors for the original matrices
  CUBLAS_CALL_THROW(cublasLtMatrixLayoutCreate(&a_desc, CUDA_R_8I, m, k, lda));
  CUBLAS_CALL_THROW(cublasLtMatrixLayoutCreate(&b_desc, CUDA_R_8I, n, k, ldb));
  CUBLAS_CALL_THROW(cublasLtMatrixLayoutCreate(&c_desc, CUDA_R_32I, m, n, ldc));

  // Create descriptors for the transformed matrices
  CUBLAS_CALL_THROW(cublasLtMatrixLayoutCreate(&AtransformDesc, CUDA_R_8I, m, k, ldatransform));
  CUBLAS_CALL_THROW(cublasLtMatrixLayoutSetAttribute(AtransformDesc, CUBLASLT_MATRIX_LAYOUT_ORDER, &order_COL32, sizeof(order_COL32)));

  CUBLAS_CALL_THROW(cublasLtMatrixLayoutCreate(&BtransformDesc, CUDA_R_8I, n, k, ldbtransform));
  CUBLAS_CALL_THROW(cublasLtMatrixLayoutSetAttribute(BtransformDesc, CUBLASLT_MATRIX_LAYOUT_ORDER, &order_COL4_4R2_8C, sizeof(order_COL4_4R2_8C)));

  CUBLAS_CALL_THROW(cublasLtMatrixLayoutCreate(&CtransformDesc, CUDA_R_32I, m, n, ldctransform));
  CUBLAS_CALL_THROW(cublasLtMatrixLayoutSetAttribute(CtransformDesc, CUBLASLT_MATRIX_LAYOUT_ORDER, &order_COL32, sizeof(order_COL32)));

  // Transforms and computation
  CUBLAS_CALL_THROW(cublasLtMatrixTransform(ltHandle,
                                            transform_desc,
                                            &transformAlpha,
                                            A,
                                            a_desc,
                                            &transformBeta,
                                            NULL,
                                            NULL,
                                            a_transform.get(),
                                            AtransformDesc,
                                            0));
  CUBLAS_CALL_THROW(cublasLtMatrixTransform(ltHandle,
                                            transform_desc,
                                            &transformAlpha,
                                            B,
                                            b_desc,
                                            &transformBeta,
                                            NULL,
                                            NULL,
                                            b_transform.get(),
                                            BtransformDesc,
                                            0));

  // No need to transform C matrix as beta is assumed to be 0
  CUBLAS_CALL_THROW(cublasLtMatmul(ltHandle,
                                   matmulDesc,
                                   &alpha,
                                   a_transform.get(),
                                   AtransformDesc,
                                   b_transform.get(),
                                   BtransformDesc,
                                   &beta,
                                   c_transform.get(),
                                   CtransformDesc,
                                   c_transform.get(),
                                   CtransformDesc,
                                   NULL,
                                   NULL,
                                   0,
                                   0));

  //// Transform the outputs to COL order
  CUBLAS_CALL_THROW(cublasLtMatrixTransform(ltHandle,
                                            transform_desc,
                                            &transformAlpha,
                                            c_transform.get(),
                                            CtransformDesc,
                                            &transformBeta,
                                            NULL,
                                            NULL,
                                            C,
                                            c_desc,
                                            0));

  // Descriptors are no longer needed as all GPU work was already
  // enqueued.
  if (CtransformDesc) cublasLtMatrixLayoutDestroy(CtransformDesc);
  if (BtransformDesc) cublasLtMatrixLayoutDestroy(BtransformDesc);
  if (AtransformDesc) cublasLtMatrixLayoutDestroy(AtransformDesc);
  if (c_desc) cublasLtMatrixLayoutDestroy(c_desc);
  if (b_desc) cublasLtMatrixLayoutDestroy(b_desc);
  if (a_desc) cublasLtMatrixLayoutDestroy(a_desc);
  if (matmulDesc) cublasLtMatmulDescDestroy(matmulDesc);
  if (transform_desc) cublasLtMatrixTransformDescDestroy(transform_desc);
}
}
}