
#ifdef _MSC_VER
#pragma warning(disable: 4505) // warning C4505: '__float2half_rz': unreferenced local function has been removed (missing 'static inline')
#endif

#include <cublas_v2.h>
#include <cusparse.h>

// clang-format off
#include "tensors/gpu/prod.h"
#include "tensors/gpu/backend.h"
#include "tensors/gpu/cuda_helpers.h"
// clang-format on

namespace marian {

namespace gpu {

static void setTensorMode(cublasHandle_t cublasHandle) {
  cublasHandle; // fool warnings
#if CUDA_VERSION >= 9000
  static int mode = 0;  // 1: use TC; -1: do not use TC; 0: not set yet
  if (mode == 0) { // multi-thread note: this is sort-of thread-safe, since multiple threads would determine the same value
    const char* var = getenv("ENABLE_CUBLAS_TENSOR_OP_MATH_FP32");
    if (!var)
      var = "1";
    switch(var[0]) {
    case '0': mode = -1; break;
    case '1': mode =  1; break;
    default: ABORT("Invalid ENABLE_CUBLAS_TENSOR_OP_MATH_FP32={}", var);
    }
    if (mode > 0) { // try whether it can be set   --@TODO: check whether this actually works
      CUBLAS_CHECK(cublasSetMathMode(cublasHandle, CUBLAS_TENSOR_OP_MATH));
      cublasMath_t actual = CUBLAS_DEFAULT_MATH;
      cublasGetMathMode(cublasHandle, &actual);
      if (actual != CUBLAS_TENSOR_OP_MATH) {
        LOG(warn, "[gpu] TensorCores requested but not available");
        mode = -1;
      }
    }
    if (mode > 0)
      LOG(info, "[gpu] 16-bit TensorCores enabled for float32 matrix operations");
  }
  CUBLAS_CHECK(cublasSetMathMode(cublasHandle, mode > 0 ? CUBLAS_TENSOR_OP_MATH : CUBLAS_DEFAULT_MATH));
#endif
}

static void unsetTensorMode(cublasHandle_t cublasHandle) {
  cublasHandle; // fool warnings
#if CUDA_VERSION >= 9000
  CUBLAS_CHECK(cublasSetMathMode(cublasHandle, CUBLAS_DEFAULT_MATH));
#endif
}

void Prod(marian::Tensor C,
          const marian::Tensor& A,
          const marian::Tensor& B,
          bool transA,
          bool transB,
          float beta,
          float scalar) {
  cudaSetDevice((int)C->getDeviceId().no);
  float alpha = scalar;

  size_t m = A->shape().elements() / A->shape().back();
  size_t k = A->shape().back();
  if(transA)
    std::swap(m, k);

  size_t l = B->shape().elements() / B->shape().back();
  size_t n = B->shape().back();
  if(transB)
    std::swap(l, n);

  size_t lda = A->shape().back();
  size_t ldb = B->shape().back();
  size_t ldc = B->shape().back();

  if(transB)
    ldc = B->shape().elements() / B->shape().back();

  cublasOperation_t opA = transA ? CUBLAS_OP_T : CUBLAS_OP_N;
  cublasOperation_t opB = transB ? CUBLAS_OP_T : CUBLAS_OP_N;

  auto cublasHandle = std::static_pointer_cast<gpu::Backend>(C->getBackend())
                          ->getCublasHandle();

  setTensorMode(cublasHandle);
  CUBLAS_CHECK(cublasSgemm(cublasHandle,
              opB,
              opA,
              (int)n,
              (int)m,
              (int)k,
              &alpha,
              B->data(),
              (int)ldb,
              A->data(),
              (int)lda,
              &beta,
              C->data(),
              (int)ldc));
  unsetTensorMode(cublasHandle);
}

void ProdBatched(marian::Tensor C,
                 Ptr<Allocator> allocator,
                 const marian::Tensor A,
                 const marian::Tensor B,
                 bool transA,
                 bool transB,
                 float beta,
                 float scalar) {
  cudaSetDevice((int)C->getDeviceId().no);
  float alpha = scalar;

  size_t batchA = A->shape().elements() / (A->shape()[-1] * A->shape()[-2]);
  size_t batchB = B->shape().elements() / (B->shape()[-1] * B->shape()[-2]);

  size_t m = A->shape()[-2];
  size_t k = A->shape()[-1];
  if(transA)
    std::swap(m, k);

  size_t l = B->shape()[-2];
  size_t n = B->shape()[-1];
  if(transB)
    std::swap(l, n);

  size_t lda = A->shape()[-1];
  size_t ldb = B->shape()[-1];
  size_t ldc = B->shape()[-1];

  if(transB)
    ldc = B->shape()[-2];

  cublasOperation_t opA = transA ? CUBLAS_OP_T : CUBLAS_OP_N;
  cublasOperation_t opB = transB ? CUBLAS_OP_T : CUBLAS_OP_N;

  auto cublasHandle = std::static_pointer_cast<gpu::Backend>(C->getBackend())
                          ->getCublasHandle();

  auto strideA = batchA == 1 ? 0 : m * k;
  auto strideB = batchB == 1 ? 0 : n * k;
  auto strideC = n * m;
  auto batchC = std::max(batchA, batchB);

  std::vector<const float*> aptr;
  std::vector<const float*> bptr;
  std::vector<float*> cptr;

  for(int i = 0; i < batchC; i++) {
    aptr.push_back(A->data() + (i % batchA) * strideA);
    bptr.push_back(B->data() + (i % batchB) * strideB);
    cptr.push_back(C->data() + i * strideC);
  }

  auto mp_aptr = allocator->alloc<const float*>(aptr.size());
  CudaCopy(
      aptr.data(), aptr.data() + aptr.size(), mp_aptr->data<const float*>());

  auto mp_bptr = allocator->alloc<const float*>(bptr.size());
  CudaCopy(
      bptr.data(), bptr.data() + bptr.size(), mp_bptr->data<const float*>());

  auto mp_cptr = allocator->alloc<float*>(cptr.size());
  CudaCopy(cptr.data(), cptr.data() + cptr.size(), mp_cptr->data<float*>());

  setTensorMode(cublasHandle);
  CUBLAS_CHECK(cublasSgemmBatched(cublasHandle,
                     opB,
                     opA,
                     (int)n,
                     (int)m,
                     (int)k,
                     &alpha,
                     mp_bptr->data<const float*>(),
                     (int)ldb,
                     mp_aptr->data<const float*>(),
                     (int)lda,
                     &beta,
                     mp_cptr->data<float*>(),
                     (int)ldc,
                     (int)batchC));
  unsetTensorMode(cublasHandle);

  allocator->free(mp_aptr);
  allocator->free(mp_bptr);
  allocator->free(mp_cptr);
}

// bug in cuSparse: sparse matrix is limited to 65535 columns
// This function is a drop-in replacement that handles it (by slicing).
cusparseStatus_t
static cusparseSgemmiEx(cusparseHandle_t handle, int m,
  int n, // the offending number of columns of matrices B and C
  int k, int nnz, const float *alpha, const float *A, int lda,
  const float *cscValB, const int *cscColPtrB, const int *cscRowIndB, const float *beta,
  float *C, int ldc)
{
  const int nMax = 65535; // max. number of columns allowed by cuSparse 10 implementation
  for (int j0 = 0; j0 < n; j0 += 65535) { // loop over column slices, j0 = index of first column
    // Call original function on a column slice.
    // Replace all parameters that relate to the column slice.
    // nnz does not need to be corrected.
    auto n1 = std::min(n - j0, nMax);   // width of column slice is limited to max
    auto C1 = C + j0 * ldc;             // column slice into result matrix C
    auto cscColPtrB1 = cscColPtrB + j0; // column slice into sparse factor B
    auto rc = cusparseSgemmi(handle, m, n1, k, nnz, alpha, A, lda, cscValB, cscColPtrB1, cscRowIndB, beta, C1, ldc);
    if (rc != CUSPARSE_STATUS_SUCCESS)
      return rc;
  }
  return CUSPARSE_STATUS_SUCCESS;
}

// C = op(S) x D if not swapOperands else C = D x op(S)
// op(S) = S if not transA else S^T
void CSRProd(marian::Tensor C,
             Ptr<Allocator> allocator,
             const marian::Tensor& S_values,
             const marian::Tensor& S_indices,
             const marian::Tensor& S_offsets,
             const marian::Tensor& D,
             bool transS,
             bool swapOperands,
             float beta) {
  cudaSetDevice((int)C->getDeviceId().no);
  auto cusparseHandle = std::static_pointer_cast<gpu::Backend>(C->getBackend())
                              ->getCusparseHandle();
  // interpret tensor dimensions as matrix dimensions
  const auto& shapeC = C->shape();
  const auto& shapeD = D->shape();
  // If swapOperands, S and D are swapped (C = D x S instead of C = S x D).
  // In that case, in the next 6 lines, please read all dimensions as if they were reversed in order.
  auto rowsC = shapeC[-(int)swapOperands];
  auto colsC = shapeC.elements() / rowsC;
  auto rowsD = shapeD[-(int)swapOperands];
  auto colsD = shapeD.elements() / rowsD;
  auto rowsS = transS ? rowsD : rowsC;
  auto colsS = transS ? rowsC : rowsD;
  ABORT_IF(colsD != colsC, "Inconsistent outer dimensions in CSR product");
  if (swapOperands) { // make rowsX actual row dimensions again, likewise colsX
    std::swap(rowsC, colsC);
    std::swap(rowsD, colsD);
    std::swap(rowsS, colsS);
  }
  // sparse arrays
  auto numValues  = S_values->shape().elements();
  auto numOffsets = S_offsets->shape().elements() - 1; // -1 since last value is length
  ABORT_IF(numOffsets != rowsS, "Unexpected number of rows in CSR argument");
  ABORT_IF(S_values->shape() != S_indices->shape(), "CSR values and indices must have the same size");
  float alpha = 1;
  Ptr<MemoryPiece> St_values, St_indices, St_offsets;
  if (transS != swapOperands) {
    // Cusparse gemmi() does not support this specific version of transpose, and csrmm() is non-deterministic.
    // Hence, we transpose the matrix explicitly.
    // Note that gemmi() expects a CSC, while csrmm() a CSR; hence, the strange condition (transS != swapOperands) above.
    St_values  = allocator->alloc<float>(numValues);
    St_indices = allocator->alloc<int>(numValues);
    St_offsets = allocator->alloc<int>(colsS + 1);
    // transpose the second argument
    CUSPARSE_CHECK(cusparseScsr2csc(cusparseHandle,
        /*m=*/ rowsS, // number of rows of matrix
        /*n=*/ colsS, // number of columns of matrix
        /*nnz=*/ (int)numValues,
        /*csrcVal=*/          S_values ->data<float>(),
        /*csrcRowPtr=*/ (int*)S_offsets->data<IndexType>(),
        /*csrcColInd=*/ (int*)S_indices->data<IndexType>(),
        /*cscVal=*/    St_values ->data<float>(),  // transposed version goes here
        /*cscRowInd=*/ St_indices->data<int>(),
        /*cscColPtr=*/ St_offsets->data<int>(),
        /*copyValues=*/ CUSPARSE_ACTION_NUMERIC,
        /*idxBase=*/ CUSPARSE_INDEX_BASE_ZERO));
    std::swap(rowsS, colsS); // these variables now represent the dims of the explicitly transposed object
  }
  if (swapOperands) {
    // C = D x S for row-major matrices
    // Implemented via cusparse as C' = S' x D' ("csrmm") where C' and D' are column-major,
    // and S' is CSR (if not transS then we make a transposed copy).
    cusparseMatDescr_t descrA;
    CUSPARSE_CHECK(cusparseCreateMatDescr(&descrA));
    cusparseSetMatType     (descrA, CUSPARSE_MATRIX_TYPE_GENERAL);
    cusparseSetMatIndexBase(descrA, CUSPARSE_INDEX_BASE_ZERO);
    CUSPARSE_CHECK(cusparseScsrmm(cusparseHandle,
        CUSPARSE_OPERATION_NON_TRANSPOSE, // (we explicitly transposed above)
        /*m=*/ rowsS, // #rows of first (CSR) factor (the transpose was done explicitly)
        /*n=*/ rowsC, // #cols of second (col-major) factor and (col-major) result = #rows of row-major C
        /*k=*/ colsS, // #cols of first (CSR) factor
        /*nnz=*/ (int)numValues,
        &alpha, descrA,
        /*csrValA=*/    St_values  ? St_values ->data<float>() :       S_values ->data<float>(),
        /*csrRowPtrA=*/ St_offsets ? St_offsets->data<int>()   : (int*)S_offsets->data<IndexType>(),
        /*csrColIndA=*/ St_indices ? St_indices->data<int>()   : (int*)S_indices->data<IndexType>(),
        D->data(),
        /*ldb=*/ colsD, // stride
        &beta,
        C->data(),
        /*ldc=*/ colsC)); // stride
    cusparseDestroyMatDescr(descrA);
  }
  else {
    // C = S x D for row-major matrices
    // Implemented via cusparse as C' = D' x S' ("gemmi") where C' and D' are column-major.
    CUSPARSE_CHECK(cusparseSgemmiEx(cusparseHandle,
        /*m=*/ colsD, // #rows of first (col-major) factor = #cols of row-major D
        /*n=*/ rowsC, // #cols of second (CSC) factor and (col-major) result = #rows of row-major C
        /*k=*/ rowsD, // #cols of first (col-major) factor = #rows of row-major D
        /*nnz=*/ (int)numValues,
        &alpha,
        /*A=*/ D->data(),
        /*lda=*/ colsD, // stride
        /*cscValB=*/    St_values  ? St_values ->data<float>() :       S_values ->data<float>(),
        /*cscColPtrB=*/ St_offsets ? St_offsets->data<int>()   : (int*)S_offsets->data<IndexType>(),
        /*cscRowIndB=*/ St_indices ? St_indices->data<int>()   : (int*)S_indices->data<IndexType>(),
        &beta,
        C->data(),
        /*ldc=*/ colsC)); // stride
    // Note: cuSparse 10 docs says this about cscColPtrB:
    //   "integer array of k + 1 elements that contains the start of every row and the end of the last row plus one."
    // This is wrong. It should be col instead of row, and n instead of k.
  }
  if(St_values ) allocator->free(St_values );
  if(St_indices) allocator->free(St_indices);
  if(St_offsets) allocator->free(St_offsets);
}

}  // namespace gpu
}  // namespace marian
