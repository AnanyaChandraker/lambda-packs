#pragma once

#include "ATen/Tensor.h"
#include "ATen/core/TensorImpl.h"
#include "c10/util/Exception.h"

namespace at {
struct CAFFE2_API SparseTensorImpl : public TensorImpl {
  // Stored in COO format, indices + values.

  // INVARIANTS:
  // sparse_dim: range [0, len(shape)]; sparse_dim + dense_dim = len(shape)
  // dense_dim : range [0, len(shape)]; sparse_dim + dense_dim = len(shape)
  // _indices.shape: dimensionality: 2,  shape: (sparse_dim, nnz)
  // _values.shape:  dimensionality: 1 + dense_dim.  shape: (nnz, shape[sparse_dim:])

  // The true size of the sparse tensor (e.g., if you called to_dense()
  // on it).  When THTensor merges into TensorImpl, this field
  // should move to the parent class.
  std::vector<int64_t> size_;

  int64_t sparse_dim_ = 0; // number of sparse dimensions
  int64_t dense_dim_ = 0; // number of dense dimensions

  Tensor indices_; // always a LongTensor
  Tensor values_;

  // A sparse tensor is 'coalesced' if every index occurs at most once in
  // the indices tensor, and the indices are in sorted order.  (This means
  // that it is very easy to convert a coalesced tensor to CSR format: you
  // need only compute CSR format indices.)
  //
  // Most math operations can only be performed on coalesced sparse tensors,
  // because many algorithms proceed by merging two sorted lists (of indices).
  bool coalesced_ = false;

public:
  // Public for now...
  explicit SparseTensorImpl(at::TensorTypeId, const caffe2::TypeMeta&);

  int64_t nnz() const { return values_.size(0); }
  int64_t sparse_dim() const { return sparse_dim_; }
  int64_t dense_dim() const { return dense_dim_; }
  bool coalesced() const { return coalesced_; }
  Tensor indices() const { return indices_; }
  Tensor values() const { return values_; }

  IntList sizes() const override;
  IntList strides() const override;
  bool is_contiguous() const override;
  int64_t size(int64_t d) const override;
  int64_t stride(int64_t d) const override;
  void resize_dim(int64_t ndim) override;
  void set_size(int64_t dim, int64_t new_size) override;
  void set_stride(int64_t dim, int64_t new_stride) override;
  void set_storage_offset(int64_t storage_offset) override;

  int64_t dim() const override;
  TensorImpl* maybe_zero_dim(bool condition_when_zero_dim) override;
  const Storage& storage() const override;
  int64_t storage_offset() const override;

  // WARNING: This function does NOT preserve invariants of sparse_dim/dense_dim with
  // respect to indices and values
  void raw_resize_(int64_t sparse_dim, int64_t dense_dim, IntList size) {
    size_ = size.vec();
    sparse_dim_ = sparse_dim;
    dense_dim_ = dense_dim;
    refresh_numel();
  }

  // NOTE: This function preserves invariants of sparse_dim/dense_dim with respect to
  // indices and values.
  //
  // NOTE: This function supports the following cases:
  // 1. When we keep the number of dense dimensions unchanged, and NOT shrinking the size of
  // any of the dense dimensions.
  // 2. When we keep the number of sparse dimensions unchanged, and NOT shrinking the size of
  // any of the sparse dimensions.
  // 3. When the sparse tensor has zero nnz, in which case we are free to change the shapes of
  // both its sparse and dense dimensions.
  //
  // This function DOESN'T support (and will throw an error) the following cases:
  // 1. When we attempt to change the number of sparse dimensions on a non-empty sparse tensor
  // (such an operation will invalidate the indices stored).
  // 2. When we attempt to change the number of dense dimensions on a non-empty sparse tensor
  // (such an operation will behave differently from an equivalent dense tensor's resize method,
  // and for API consistency we don't support it).
  // 3. When we attempt to shrink the size of any of the dense dimensions on a non-empty sparse tensor
  // (such an operation will behave differently from an equivalent dense tensor's resize method,
  // and for API consistency we don't support it).
  // 4. When we attempt to shrink the size of any of the sparse dimensions on a non-empty sparse tensor
  // (this could make some of the stored indices out-of-bound and thus unsafe).
  void resize_(int64_t sparse_dim, int64_t dense_dim, IntList size) {
    AT_CHECK(sparse_dim + dense_dim == size.size(), "number of dimensions must be sparse_dim (", sparse_dim, ") + dense_dim (", dense_dim, "), but got ", size.size());
    if (nnz() > 0) {
      auto alt_options_msg = "You could try the following options:\n\
1. If you need an empty sparse tensor of this size, call `x = torch.sparse_coo_tensor(size)`.\n\
2. If you need to resize this tensor, you have the following options:\n\
    1. For both sparse and dense dimensions, keep the number of them constant and the size of them non-shrinking, and then try the same call again.\n\
    2. Or, create a new sparse tensor with the correct indices and values from this sparse tensor.";

      AT_CHECK(sparse_dim == sparse_dim_,
        "changing the number of sparse dimensions (from ", sparse_dim_, " to ", sparse_dim, ") on a non-empty sparse tensor is not supported.\n", alt_options_msg);

      AT_CHECK(dense_dim == dense_dim_,
        "changing the number of dense dimensions (from ", dense_dim_, " to ", dense_dim, ") on a non-empty sparse tensor is not supported.\n", alt_options_msg);

      bool shrinking_sparse_dims = false;
      bool shrinking_dense_dim = false;
      auto sparse_size_original = sizes().slice(0, sparse_dim);
      auto sparse_size_new = size.slice(0, sparse_dim);
      for (int i = 0; i < sparse_dim; i++) {
        if (sparse_size_new[i] < sparse_size_original[i]) {
          shrinking_sparse_dims = true;
          break;
        }
      }
      auto dense_size_original = sizes().slice(sparse_dim);
      auto dense_size_new = size.slice(sparse_dim);
      for (int i = 0; i < dense_dim; i++) {
        if (dense_size_new[i] < dense_size_original[i]) {
          shrinking_dense_dim = true;
          break;
        }
      }

      AT_CHECK(!shrinking_sparse_dims,
        "shrinking the size of sparse dimensions (from ", sparse_size_original, " to ", sparse_size_new, ") on a non-empty sparse tensor is not supported.\n", alt_options_msg);

      AT_CHECK(!shrinking_dense_dim,
        "shrinking the size of dense dimensions (from ", dense_size_original, " to ", dense_size_new, ") on a non-empty sparse tensor is not supported.\n", alt_options_msg);
    }

    if ((!size.equals(size_)) || (sparse_dim != sparse_dim_) || (dense_dim != dense_dim_)) {
      auto nnz = values().size(0);
      std::vector<int64_t> values_size = {nnz};
      auto dense_size = size.slice(sparse_dim);
      values_size.insert(values_size.end(), dense_size.begin(), dense_size.end());
      values_.resize_(values_size);
      indices_.resize_({sparse_dim, nnz});
    }

    size_ = size.vec();
    sparse_dim_ = sparse_dim;
    dense_dim_ = dense_dim;
    refresh_numel();
  }

  // NOTE: this function will resize the sparse tensor and also set `indices` and `values` to empty.
  void resize_and_clear_(int64_t sparse_dim, int64_t dense_dim, IntList size) {
    AT_CHECK(sparse_dim + dense_dim == size.size(), "number of dimensions must be sparse_dim (", sparse_dim, ") + dense_dim (", dense_dim, "), but got ", size.size());

    size_ = size.vec();
    sparse_dim_ = sparse_dim;
    dense_dim_ = dense_dim;

    auto empty_indices = at::empty({sparse_dim, 0}, indices().options());
    std::vector<int64_t> values_size = {0};
    auto dense_size = sizes().slice(sparse_dim);
    values_size.insert(values_size.end(), dense_size.begin(), dense_size.end());
    auto empty_values = at::empty(values_size, values().options());
    set_indices_and_values_unsafe(empty_indices, empty_values);
    refresh_numel();
  }

  void set_coalesced(bool coalesced) { coalesced_ = coalesced; }

  // NOTE: this function is only used internally and not exposed to Python frontend
  void set_nnz_and_narrow(int64_t new_nnz) {
    AT_ASSERT(new_nnz <= nnz());
    indices_ = indices_.narrow(1, 0, new_nnz);
    values_ = values_.narrow(0, 0, new_nnz);
  }

  // Takes indices and values and directly puts them into the sparse tensor, no copy.
  // NOTE: this function is unsafe because it doesn't check whether any indices are
  // out of boundaries of `sizes`, so it should ONLY be used where we know that the
  // indices are guaranteed to be within bounds.
  // This used to be called THSTensor_(_move)
  // NB: This used to be able to avoid a refcount bump, but I was too lazy to
  // make it happen
  void set_indices_and_values_unsafe(const Tensor& indices, const Tensor& values);

 private:
  int64_t get_device_slow() const override {
    return values_.get_device();
  }

};

} // namespace at