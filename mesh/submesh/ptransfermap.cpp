// Copyright (c) 2010-2025, Lawrence Livermore National Security, LLC. Produced
// at the Lawrence Livermore National Laboratory. All Rights reserved. See files
// LICENSE and NOTICE for details. LLNL-CODE-806117.
//
// This file is part of the MFEM library. For more information and source code
// availability visit https://mfem.org.
//
// MFEM is free software; you can redistribute it and/or modify it under the
// terms of the BSD-3 license. We welcome feedback and contributions, see file
// CONTRIBUTING.md for details.

#include "../../config/config.hpp"

#ifdef MFEM_USE_MPI

#include "psubmesh.hpp"
#include "ptransfermap.hpp"
#include "submesh_utils.hpp"

using namespace mfem;

ParTransferMap::ParTransferMap(const ParFiniteElementSpace &src,
                               const ParFiniteElementSpace &dst)
{
   if (ParSubMesh::IsParSubMesh(src.GetParMesh()) &&
       ParSubMesh::IsParSubMesh(dst.GetParMesh()))
   {
      ParSubMesh* src_sm = static_cast<ParSubMesh*>(src.GetParMesh());
      ParSubMesh* dst_sm = static_cast<ParSubMesh*>(dst.GetParMesh());

      // There is no immediate relation and both src and dst come from a
      // SubMesh, check if they have an equivalent root parent.
      if (SubMeshUtils::GetRootParent(*src_sm) !=
          SubMeshUtils::GetRootParent(*dst_sm))
      {
         MFEM_ABORT("Can't find a relation between the two GridFunctions");
      }

      category_ = TransferCategory::SubMeshToSubMesh;

      {
         ParMesh * parent_mesh =
            const_cast<ParMesh *>(SubMeshUtils::GetRootParent(*src_sm));

         int parent_dim = parent_mesh->Dimension();
         int src_sm_dim = src_sm->Dimension();
         int dst_sm_dim = dst_sm->Dimension();

         bool root_fes_reset = false;
         if (src_sm_dim == parent_dim - 1 && dst_sm_dim == parent_dim - 1)
         {
            const FiniteElementCollection *src_fec = src.FEColl();
            const FiniteElementCollection *dst_fec = dst.FEColl();

            const L2_FECollection *src_l2_fec =
               dynamic_cast<const L2_FECollection*>(src_fec);
            const L2_FECollection *dst_l2_fec =
               dynamic_cast<const L2_FECollection*>(dst_fec);

            if (src_l2_fec != NULL && dst_l2_fec != NULL)
            {
               // Source and destination are both lower dimension L2 spaces.
               // Transfer them as the trace of an RT space if possible.

               int src_mt = src_fec->GetMapType(src_sm_dim);
               int dst_mt = dst_fec->GetMapType(dst_sm_dim);

               int src_bt = src_l2_fec->GetBasisType();
               int dst_bt = dst_l2_fec->GetBasisType();

               int src_p = src_fec->GetOrder();
               int dst_p = dst_fec->GetOrder();

               if (src_mt == FiniteElement::INTEGRAL &&
                   dst_mt == FiniteElement::INTEGRAL &&
                   src_bt == BasisType::GaussLegendre &&
                   dst_bt == BasisType::GaussLegendre &&
                   src_p == dst_p)
               {
                  // The subspaces are consistent with the trace of an RT space
                  root_fec_.reset(new RT_FECollection(src_p, parent_dim));
                  root_fes_.reset(new ParFiniteElementSpace(
                                     const_cast<ParMesh *>(
                                        SubMeshUtils::GetRootParent(*src_sm)),
                                     root_fec_.get()));
                  root_fes_reset = true;
               }
            }
         }

         if (!root_fes_reset)
         {
            root_fes_.reset(new ParFiniteElementSpace(
                               src,
                               const_cast<ParMesh *>(
                                  SubMeshUtils::GetRootParent(*src_sm))));
         }
      }

      src_to_parent.reset(new ParTransferMap(src, *root_fes_));
      dst_to_parent.reset(new ParTransferMap(dst, *root_fes_));
      parent_to_dst.reset(new ParTransferMap(*root_fes_, dst));

      z_.SetSpace(root_fes_.get());
   }
   else if (ParSubMesh::IsParSubMesh(src.GetParMesh()))
   {
      category_ = TransferCategory::SubMeshToParent;
      ParSubMesh* src_sm = static_cast<ParSubMesh*>(src.GetParMesh());
      SubMeshUtils::BuildVdofToVdofMap(src,
                                       dst,
                                       src_sm->GetFrom(),
                                       src_sm->GetParentElementIDMap(),
                                       sub_to_parent_map_);

      root_gc_ = &dst.GroupComm();
      CommunicateIndicesSet(sub_to_parent_map_, dst.GetVSize());
      sub_fes_ = &src;
   }
   else if (ParSubMesh::IsParSubMesh(dst.GetParMesh()))
   {
      category_ = TransferCategory::ParentToSubMesh;
      ParSubMesh* dst_sm = static_cast<ParSubMesh*>(dst.GetParMesh());
      SubMeshUtils::BuildVdofToVdofMap(dst,
                                       src,
                                       dst_sm->GetFrom(),
                                       dst_sm->GetParentElementIDMap(),
                                       sub_to_parent_map_);
      sub_fes_ = &dst;
   }
   else
   {
      MFEM_ABORT("Trying to do a transfer between GridFunctions but none of them is defined on a SubMesh");
   }
}

ParTransferMap::ParTransferMap(const ParGridFunction &src,
                               const ParGridFunction &dst)
   : ParTransferMap(*src.ParFESpace(), *dst.ParFESpace())
{ }

void ParTransferMap::Transfer(const ParGridFunction &src,
                              ParGridFunction &dst) const
{
   if (category_ == TransferCategory::ParentToSubMesh)
   {
      // dst = S1^T src
      src.HostRead();
      dst.HostWrite(); // dst is fully overwritten
      for (int i = 0; i < sub_to_parent_map_.Size(); i++)
      {
         real_t s = 1.0;
         int j = FiniteElementSpace::DecodeDof(sub_to_parent_map_[i], s);
         dst(i) = s * src(j);
      }

      CorrectFaceOrientations(*sub_fes_, src, dst);
   }
   else if (category_ == TransferCategory::SubMeshToParent)
   {
      // dst = G S1 src
      //     = G z
      //
      // G is identity if the partitioning matches

      src.HostRead();
      dst.HostReadWrite(); // dst is only partially overwritten
      for (int i = 0; i < sub_to_parent_map_.Size(); i++)
      {
         real_t s = 1.0;
         int j = FiniteElementSpace::DecodeDof(sub_to_parent_map_[i], s);
         dst(j) = s * src(i);
      }

      CorrectFaceOrientations(*sub_fes_, src, dst,
                              &sub_to_parent_map_);

      CommunicateSharedVdofs(dst);
   }
   else if (category_ == TransferCategory::SubMeshToSubMesh)
   {
      dst_to_parent->Transfer(dst, z_);
      src_to_parent->Transfer(src, z_);
      parent_to_dst->Transfer(z_, dst);
   }
   else
   {
      MFEM_ABORT("unknown TransferCategory: " << category_);
   }
}

void ParTransferMap::CommunicateIndicesSet(Array<int> &map, int dst_sz)
{
   indices_set_local_.SetSize(dst_sz);
   indices_set_local_ = 0;
   for (int i = 0; i < map.Size(); i++)
   {
      indices_set_local_[(map[i]>=0)?map[i]:(-map[i]-1)] = 1;
   }
   indices_set_global_ = indices_set_local_;
   root_gc_->Reduce(indices_set_global_, GroupCommunicator::Sum);
   root_gc_->Bcast(indices_set_global_);
}

void ParTransferMap::CommunicateSharedVdofs(Vector &f) const
{
   // f is usually defined on the root vdofs

   const Table &group_ldof = root_gc_->GroupLDofTable();

   // Identify indices that were only set by other ranks and clear the dof.
   for (int i = 0; i < group_ldof.Size_of_connections(); i++)
   {
      const int j = group_ldof.GetJ()[i];
      if (indices_set_global_[j] != 0 && indices_set_local_[j] == 0)
      {
         f(j) = 0.0;
      }
   }

   // TODO: do the reduce only on dofs of interest
   root_gc_->Reduce<real_t>(f.HostReadWrite(), GroupCommunicator::Sum);

   // Indices that were set from this rank or other ranks have been summed up
   // and therefore need to be "averaged". Note that this results in the exact
   // value that is desired.
   for (int i = 0; i < group_ldof.Size_of_connections(); i++)
   {
      const int j = group_ldof.GetJ()[i];
      if (indices_set_global_[j] != 0)
      {
         f(j) /= indices_set_global_[j];
      }
   }

   // Indices for dofs that are shared between processors need to be divided by
   // the whole group size that share this dof.
   for (int gr = 1; gr < group_ldof.Size(); gr++)
   {
      for (int i = 0; i < group_ldof.RowSize(gr); i++)
      {
         const int j = group_ldof.GetRow(gr)[i];
         if (indices_set_global_[j] == 0)
         {
            f(j) /= root_gc_->GetGroupTopology().GetGroupSize(gr);
         }
      }
   }

   root_gc_->Bcast<real_t>(f.HostReadWrite());
}

void
ParTransferMap::CorrectFaceOrientations(const ParFiniteElementSpace &fes,
                                        const Vector &src,
                                        Vector &dst,
                                        const Array<int> *sub_to_parent_map)
{
   const FiniteElementCollection * fec = fes.FEColl();

   ParSubMesh * mesh = dynamic_cast<ParSubMesh*>(fes.GetParMesh());

   const Array<int>& parent_face_ori = mesh->GetParentFaceOrientations();

   if (parent_face_ori.Size() == 0) { return; }

   DofTransformation doftrans(fes.GetVDim(), fes.GetOrdering());

   int dim = mesh->Dimension();
   bool face = (dim == 3);

   Array<int> vdofs;
   Array<int> Fo(1);
   Vector face_vector;

   for (int i = 0; i < (face ? mesh->GetNumFaces() : mesh->GetNE()); i++)
   {
      if (parent_face_ori[i] == 0) { continue; }

      Geometry::Type geom = face ? mesh->GetFaceGeometry(i) :
                            mesh->GetElementGeometry(i);

      if (!fec->DofTransformationForGeometry(geom)) { continue; }
      doftrans.SetDofTransformation(*fec->DofTransformationForGeometry(geom));

      Fo[0] = parent_face_ori[i];
      doftrans.SetFaceOrientations(Fo);

      if (face)
      {
         fes.GetFaceVDofs(i, vdofs);
      }
      else
      {
         fes.GetElementVDofs(i, vdofs);
      }

      if (sub_to_parent_map)
      {
         src.GetSubVector(vdofs, face_vector);
         doftrans.TransformPrimal(face_vector);
      }
      else
      {
         dst.GetSubVector(vdofs, face_vector);
         doftrans.InvTransformPrimal(face_vector);
      }

      for (int j = 0; j < vdofs.Size(); j++)
      {
         real_t s = 1.0;
         int k = FiniteElementSpace::DecodeDof(vdofs[j], s);

         if (sub_to_parent_map)
         {
            real_t sps = 1.0;
            int spk = FiniteElementSpace::DecodeDof((*sub_to_parent_map)[k],
                                                    sps);
            s *= sps;
            k = spk;
         }

         dst[k] = s * face_vector[j];
      }
   }
}

#endif // MFEM_USE_MPI
