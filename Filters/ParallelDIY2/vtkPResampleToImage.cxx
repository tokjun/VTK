/*=========================================================================

  Program:   Visualization Toolkit
  Module:    vtkPResampleToImage.cxx

  Copyright (c) Ken Martin, Will Schroeder, Bill Lorensen
  All rights reserved.
  See Copyright.txt or http://www.kitware.com/Copyright.htm for details.

     This software is distributed WITHOUT ANY WARRANTY; without even
     the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
     PURPOSE.  See the above copyright notice for more information.

=========================================================================*/
#include "vtkPResampleToImage.h"

#include "vtkCharArray.h"
#include "vtkDataSetAttributes.h"
#include "vtkIdList.h"
#include "vtkImageData.h"
#include "vtkInformation.h"
#include "vtkInformationVector.h"
#include "vtkMath.h"
#include "vtkNew.h"
#include "vtkObjectFactory.h"
#include "vtkPointData.h"
#include "vtkProbeFilter.h"
#include "vtkStreamingDemandDrivenPipeline.h"
#include "vtkUnsignedCharArray.h"

#include "vtk_diy2.h"   // must include this before any diy header
#include VTK_DIY2_HEADER(diy/algorithms.hpp)
#include VTK_DIY2_HEADER(diy/assigner.hpp)
#include VTK_DIY2_HEADER(diy/link.hpp)
#include VTK_DIY2_HEADER(diy/master.hpp)
#include VTK_DIY2_HEADER(diy/mpi.hpp)

#include <mpi.h>

#include <algorithm>


vtkStandardNewMacro(vtkPResampleToImage);

namespace {

//----------------------------------------------------------------------------
template <typename T, std::size_t Len>
struct Array
{
public:
  T& operator[](std::size_t idx)
  {
    return this->Data[idx];
  }

  const T& operator[](std::size_t idx) const
  {
    return this->Data[idx];
  }

  T* data()
  {
    return this->Data;
  }

  const T* data() const
  {
    return this->Data;
  }

  std::size_t size() const
  {
    return Len;
  }

private:
  T Data[Len];
};


//----------------------------------------------------------------------------
struct Point
{
  double pos[3];
  int idx[3];
  std::vector<char> data;

  double& operator[](std::size_t i)
  {
    return pos[i];
  }

  double operator[](std::size_t i) const
  {
    return pos[i];
  }
};


//-----------------------------------------------------------------------------
struct FieldMetaData
{
  std::string Name;
  int DataType;
  int NumComponents;
  int AttributeType;
};

inline void ExtractFieldMetaData(vtkDataSetAttributes *data,
                                 std::vector<FieldMetaData> *metadata)
{
  std::size_t numFields = static_cast<std::size_t>(data->GetNumberOfArrays());
  metadata->resize(numFields);

  for (std::size_t i = 0; i < numFields; ++i)
    {
    FieldMetaData &md = (*metadata)[i];
    vtkDataArray *da = data->GetArray(static_cast<int>(i));

    md.Name = da->GetName();
    md.DataType = da->GetDataType();
    md.NumComponents = da->GetNumberOfComponents();
    md.AttributeType = data->IsArrayAnAttribute(static_cast<int>(i));
    }
}


inline void InitializeFieldData(const std::vector<FieldMetaData> &metadata,
                                vtkIdType numTuples,
                                vtkDataSetAttributes *data)
{
  std::size_t numFields = metadata.size();
  for (std::size_t i = 0; i < numFields; ++i)
    {
    const FieldMetaData &md = metadata[i];
    vtkDataArray *da = vtkDataArray::CreateDataArray(md.DataType);
    da->SetName(md.Name.c_str());
    da->SetNumberOfComponents(md.NumComponents);
    da->SetNumberOfTuples(numTuples);

    double null_value = 0.0;
    for (int j = 0; j < da->GetNumberOfComponents(); ++j)
      {
      da->FillComponent(j, null_value);
      }
    data->AddArray(da);
    da->Delete();

    if (md.AttributeType >= 0)
      {
      data->SetActiveAttribute(static_cast<int>(i), md.AttributeType);
      }
    }
}

inline void SerializeFieldData(vtkFieldData *field, vtkIdType tuple,
                               std::vector<char> &bytestream)
{
  diy::MemoryBuffer bb;
  int numFields = field->GetNumberOfArrays();
  for (int i = 0; i < numFields; ++i)
    {
    vtkDataArray *da = field->GetArray(i);
    void *ptr = da->GetVoidPointer(tuple);
    std::size_t numComponents = static_cast<std::size_t>(da->GetNumberOfComponents());
    switch(da->GetDataType())
      {
      vtkTemplateMacro(diy::save(bb, static_cast<VTK_TT*>(ptr), numComponents));
      }
    }
  std::swap(bytestream, bb.buffer);
}

inline void DeserializeFieldData(const std::vector<char> &bytestream,
                                 vtkFieldData *field, vtkIdType tuple)
{
  diy::MemoryBuffer bb;
  bb.buffer = bytestream;

  int numFields = field->GetNumberOfArrays();
  for (int i = 0; i < numFields; ++i)
    {
    vtkDataArray *da = field->GetArray(i);
    void *ptr = da->GetVoidPointer(tuple);
    std::size_t numComponents = static_cast<std::size_t>(da->GetNumberOfComponents());
    switch(da->GetDataType())
      {
      vtkTemplateMacro(diy::load(bb, static_cast<VTK_TT*>(ptr), numComponents));
      }
    }
}


//----------------------------------------------------------------------------
inline void ComputeBoundsFromExtent(const int extent[6], const double origin[3],
                                    const double spacing[3], double bounds[6])
{
  for (int i = 0; i < 3; ++i)
    {
    bounds[2*i] = static_cast<double>(extent[2*i]) * spacing[i] + origin[i];
    bounds[2*i + 1] = static_cast<double>(extent[2*i + 1]) * spacing[i] +
                      origin[i];
    }
}

inline void ComputeBoundingExtent(const double origin[3], const double spacing[3],
                                  const double bounds[6], int extent[6])
{
  for (int i = 0; i < 3; ++i)
    {
    extent[2*i] = static_cast<int>(
                  vtkMath::Floor((bounds[2*i] - origin[i]) / spacing[i]));
    extent[2*i + 1] = static_cast<int>(
                      vtkMath::Ceil((bounds[2*i + 1] - origin[i])/spacing[i]));
    }
}

inline void ComputeTilingExtent(const double origin[3], const double spacing[3],
                                const double bounds[6], int extent[6])
{
  for (int i = 0; i < 3; ++i)
    {
    extent[2*i] = static_cast<int>(
                  vtkMath::Floor((bounds[2*i] - origin[i])/spacing[i]));
    extent[2*i + 1] = static_cast<int>(
                      vtkMath::Floor((bounds[2*i + 1] - origin[i])/spacing[i]));
    }
}

inline bool InsideExtent(const int extent[6], const int ijk[3])
{
  return ijk[0] >= extent[0] && ijk[0] <= extent[1] &&
         ijk[1] >= extent[2] && ijk[1] <= extent[3] &&
         ijk[2] >= extent[4] && ijk[2] <= extent[5];
}


//----------------------------------------------------------------------------
struct Block
{
  std::vector<Point> Points;
};

inline void* CreateBlock()
{
  return new Block;
}

inline void DestroyBlock(void *blockp)
{
  delete static_cast<Block*>(blockp);
}

inline void GetPoint(vtkImageData *img, const int ijk[3], double x[3])
{
  double *origin = img->GetOrigin();
  double *spacing = img->GetSpacing();
  for (int i = 0; i < 3; ++i)
  {
    x[i] = origin[i] + (static_cast<double>(ijk[i]) * spacing[i]);
  }
}

inline void AddPointsToBlock(vtkImageData *piece, const char *maskArrayName,
                             Block *block)
{
  vtkPointData *pd = piece->GetPointData();
  vtkCharArray *maskArray = vtkCharArray::SafeDownCast(pd->GetArray(maskArrayName));
  char *mask = maskArray->GetPointer(0);

  int extent[6];
  piece->GetExtent(extent);
  for (int k = extent[4]; k <= extent[5]; ++k)
    {
    for (int j = extent[2]; j <= extent[3]; ++j)
      {
      for (int i = extent[0]; i <= extent[1]; ++i)
        {
        int ijk[3] = { i, j, k };
        vtkIdType id = piece->ComputePointId(ijk);
        if (mask[id])
          {
          double x[3];
          GetPoint(piece, ijk, x);

          Point pt;
          std::copy(x, x + 3, pt.pos);
          std::copy(ijk, ijk + 3, pt.idx);
          SerializeFieldData(pd, id, pt.data);
          block->Points.push_back(pt);
          }
        }
      }
    }
}

//---------------------------------------------------------------------------
class FindGhostPointsForNeighbors
{
public:
  FindGhostPointsForNeighbors(const double *origin, const double *spacing)
    : Origin(origin), Spacing(spacing)
  { }

  void operator()(void *blockp, const diy::Master::ProxyWithLink& cp, void*) const
  {
    Block &block = *static_cast<Block*>(blockp);
    diy::RegularContinuousLink &link =
        *static_cast<diy::RegularContinuousLink*>(cp.link());

    std::size_t numNeighbors = static_cast<std::size_t>(link.size());
    std::vector<Array<int, 6> > extents(numNeighbors);
    for (std::size_t i = 0; i < numNeighbors; ++i)
      {
      const diy::ContinuousBounds &b = link.bounds(static_cast<int>(i));
      double bounds[6] = { b.min[0], b.max[0], b.min[1], b.max[1], b.min[2], b.max[2] };
      ComputeTilingExtent(this->Origin, this->Spacing, bounds, extents[i].data());
      }

    for (std::size_t i = 0; i < block.Points.size(); ++i)
      {
      const Point &p = block.Points[i];
      for (std::size_t j = 0; j < extents.size(); ++j)
        {
        if (InsideExtent(extents[j].data(), p.idx))
          {
          cp.enqueue(link.target(static_cast<int>(j)), p);
          }
        }
      }
  }

private:
  const double *Origin;
  const double *Spacing;
};

class ReceiveGhostPointsAndMakeResult
{
public:
  ReceiveGhostPointsAndMakeResult(const double *origin, const double *spacing,
                                  const std::vector<FieldMetaData> &fieldMetaData,
                                  vtkImageData *result)
    : Origin(origin), Spacing(spacing), FMetaData(&fieldMetaData), Result(result)
  { }

  void operator()(void *blockp, const diy::Master::ProxyWithLink& cp, void*) const
  {
    Block &block = *static_cast<Block*>(blockp);
    diy::RegularContinuousLink &link =
        *static_cast<diy::RegularContinuousLink*>(cp.link());

    diy::Master::IncomingQueues &in = *cp.incoming();
    for (diy::Master::IncomingQueues::iterator i = in.begin(); i != in.end(); ++i)
      {
      while (i->second)
        {
        Point pt;
        cp.dequeue(i->first, pt);
        block.Points.push_back(pt);
        }
      }

    int extent[6];
    const diy::ContinuousBounds &b = link.bounds();
    double bounds[6] = { b.min[0], b.max[0], b.min[1], b.max[1], b.min[2], b.max[2] };
    ComputeTilingExtent(this->Origin, this->Spacing, bounds, extent);

    this->Result->SetOrigin(this->Origin[0], this->Origin[1], this->Origin[2]);
    this->Result->SetSpacing(this->Spacing[0], this->Spacing[1], this->Spacing[2]);
    this->Result->SetExtent(extent);

    vtkPointData *pd = this->Result->GetPointData();
    InitializeFieldData(*this->FMetaData, this->Result->GetNumberOfPoints(), pd);

    for (std::size_t i = 0; i < block.Points.size(); ++i)
      {
      Point &p = block.Points[i];
      vtkIdType id = this->Result->ComputePointId(p.idx);
      DeserializeFieldData(p.data, pd, id);
      }
  }

private:
  const double *Origin;
  const double *Spacing;
  const std::vector<FieldMetaData> *FMetaData;
  vtkImageData *Result;
};


//----------------------------------------------------------------------------
inline void ComputeGlobalBounds(const double lbounds[6], double gbounds[6])
{
  Array<double, 3> localBoundsMin, localBoundsMax;
  for (std::size_t i = 0; i < 3; ++i)
    {
    localBoundsMin[i] = lbounds[2*i];
    localBoundsMax[i] = lbounds[2*i + 1];
    }

  diy::mpi::communicator world;
  Array<double, 3> globalBoundsMin, globalBoundsMax;
  diy::mpi::all_reduce(world, localBoundsMin, globalBoundsMin,
                       diy::mpi::minimum<double>());
  diy::mpi::all_reduce(world, localBoundsMax, globalBoundsMax,
                       diy::mpi::maximum<double>());

  for (std::size_t i = 0; i < 3; ++i)
    {
    gbounds[2*i] = globalBoundsMin[i];
    gbounds[2*i + 1] = globalBoundsMax[i];
    }
}

inline void GetGlobalFieldMetaData(vtkDataSetAttributes *data,
                                   std::vector<FieldMetaData> *metadata)
{
  diy::mpi::communicator world;

  std::vector<FieldMetaData> local;
  ExtractFieldMetaData(data, &local);

  int rank = local.size() ? world.rank() : world.size();
  int source;
  diy::mpi::all_reduce(world, rank, source, diy::mpi::minimum<int>());

  diy::MemoryBuffer bb;
  if (world.rank() == source)
    {
    diy::save(bb, local);
    bb.reset();
    }
  diy::mpi::broadcast(world, bb.buffer, source);
  diy::load(bb, *metadata);
}

} // anonymous namespace


//---------------------------------------------------------------------------
vtkPResampleToImage::vtkPResampleToImage()
{
}

//----------------------------------------------------------------------------
vtkPResampleToImage::~vtkPResampleToImage()
{
}

//---------------------------------------------------------------------------
int vtkPResampleToImage::RequestData(vtkInformation *request,
                                     vtkInformationVector **inputVector,
                                     vtkInformationVector *outputVector)
{
  int mpiInitializedFlag = 0;
  MPI_Initialized(&mpiInitializedFlag);

  if (!mpiInitializedFlag || diy::mpi::communicator().size() == 1)
    {
    return this->Superclass::RequestData(request, inputVector, outputVector);
    }

  // get the info objects
  vtkInformation *inInfo = inputVector[0]->GetInformationObject(0);
  vtkInformation *outInfo = outputVector->GetInformationObject(0);

  // get the input and output
  vtkDataSet *input = vtkDataSet::SafeDownCast(
    inInfo->Get(vtkDataObject::DATA_OBJECT()));
  vtkImageData *output = vtkImageData::SafeDownCast(
    outInfo->Get(vtkDataObject::DATA_OBJECT()));

  // compute global bounds of the dataset
  double localBounds[6], globalBounds[6];
  input->GetBounds(localBounds);
  ComputeGlobalBounds(localBounds, globalBounds);

  // compute bounds and extent where probing should be performed for this node
  double *wholeBounds = this->UseInputBounds ? globalBounds : this->SamplingBounds;
  double origin[3] = { wholeBounds[0], wholeBounds[2], wholeBounds[4] };
  double spacing[3] = {
    (wholeBounds[1] - wholeBounds[0])/static_cast<double>(this->SamplingDimensions[0] - 1),
    (wholeBounds[3] - wholeBounds[2])/static_cast<double>(this->SamplingDimensions[1] - 1),
    (wholeBounds[5] - wholeBounds[4])/static_cast<double>(this->SamplingDimensions[2] - 1)
  };

  int updateExtent[6];
  if (outInfo->Has(vtkStreamingDemandDrivenPipeline::UPDATE_EXTENT()))
    {
    int *ue = outInfo->Get(vtkStreamingDemandDrivenPipeline::UPDATE_EXTENT());
    std::copy(ue, ue + 6, updateExtent);
    }
  else
    {
    updateExtent[0] = updateExtent[2] = updateExtent[4] = 0;
    updateExtent[1] = this->SamplingDimensions[0] - 1;
    updateExtent[3] = this->SamplingDimensions[1] - 1;
    updateExtent[5] = this->SamplingDimensions[2] - 1;
    }

  bool emptyExtent = false;
  int extent[6];
  ComputeBoundingExtent(origin, spacing, localBounds, extent);
  for (int i = 0; i < 3; ++i)
    {
    extent[2*i] = vtkMath::Max(extent[2*i], updateExtent[2*i]);
    extent[2*i + 1] = vtkMath::Min(extent[2*i + 1], updateExtent[2*i + 1]);
    if (extent[2*i] > extent[2*i + 1])
      {
      emptyExtent = true; // no overlap
      break;
      }
    }

  // perform probing
  vtkNew<vtkProbeFilter> prober;
  if (!emptyExtent)
    {
    vtkNew<vtkImageData> structure;
    structure->SetOrigin(origin);
    structure->SetSpacing(spacing);
    structure->SetExtent(extent);

    prober->SetInputData(structure.GetPointer());
    prober->SetSourceData(input);
    prober->Update();
    }

  const char *maskArrayName = prober->GetValidPointMaskArrayName();

  // Ensure every node has fields' metadata information
  vtkImageData *mypiece = static_cast<vtkImageData*>(prober->GetOutput());
  std::vector<FieldMetaData> fieldMetaData;
  GetGlobalFieldMetaData(mypiece->GetPointData(), &fieldMetaData);

  // perform kd-tree partitioning on probed points so that each node ends up
  // with non-overlapping rectangular regions
  diy::mpi::communicator world;
  diy::Master master(world, 1, -1, &CreateBlock, &DestroyBlock);
  diy::RoundRobinAssigner assigner(world.size(), world.size());

  std::vector<int> gids;
  assigner.local_gids(world.rank(), gids);

  double updateBounds[6];
  ComputeBoundsFromExtent(updateExtent, origin, spacing, updateBounds);
  diy::ContinuousBounds domain;
  for (int i = 0; i < 3; ++i)
    {
    domain.min[i] = static_cast<float>(updateBounds[2*i] - 1e-4);
    domain.max[i] = static_cast<float>(updateBounds[2*i + 1] + 1e-4);
    }

  // b and l are freed by master
  Block *b = new Block;
  AddPointsToBlock(mypiece, maskArrayName, b);
  diy::RegularContinuousLink *l = new diy::RegularContinuousLink(3, domain, domain);
  master.add(gids[0], b, l);

  const int hist = 32;
  diy::kdtree(master, assigner, 3, domain, &Block::Points, 2*hist, false);

  // handle ghost points
  master.foreach(FindGhostPointsForNeighbors(origin, spacing));
  master.exchange();
  master.foreach(ReceiveGhostPointsAndMakeResult(origin, spacing, fieldMetaData,
                                                 output));

  vtkPResampleToImage::SetBlankPointsAndCells(output, maskArrayName);
  return 1;
}

//----------------------------------------------------------------------------
void vtkPResampleToImage::PrintSelf(ostream& os, vtkIndent indent)
{
  this->Superclass::PrintSelf(os, indent);
}


//----------------------------------------------------------------------------
namespace diy {

namespace mpi {
namespace detail {

template <class T, std::size_t Len>
struct mpi_datatype<Array<T, Len> >
{
  typedef Array<T, Len> ArrayType;

  static MPI_Datatype datatype() { return get_mpi_datatype<T>(); }
  static const void* address(const ArrayType& x) { return &x[0]; }
  static void* address(ArrayType& x) { return &x[0]; }
  static int count(const ArrayType&) { return Len; }
};

}
} // namespace mpi::detail


template<>
struct Serialization<Point>
{
  static void save(BinaryBuffer& bb, const Point& p)
  {
    diy::save(bb, p.pos, 3);
    diy::save(bb, p.idx, 3);
    diy::save(bb, p.data);
  }

  static void load(BinaryBuffer& bb, Point& p)
  {
    diy::load(bb, p.pos, 3);
    diy::load(bb, p.idx, 3);
    diy::load(bb, p.data);
  }
};

template<>
struct Serialization<FieldMetaData>
{
  static void save(BinaryBuffer& bb, const FieldMetaData& f)
  {
    diy::save(bb, f.Name);
    diy::save(bb, f.DataType);
    diy::save(bb, f.NumComponents);
    diy::save(bb, f.AttributeType);
  }

  static void load(BinaryBuffer& bb, FieldMetaData& f)
  {
    diy::load(bb, f.Name);
    diy::load(bb, f.DataType);
    diy::load(bb, f.NumComponents);
    diy::load(bb, f.AttributeType);
  }
};

} // namespace diy
