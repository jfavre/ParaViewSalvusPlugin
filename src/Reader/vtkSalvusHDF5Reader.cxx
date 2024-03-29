#define PARALLEL_DEBUG

#include "vtkSalvusHDF5Reader.h"

#include "vtkCellArray.h"
#include "vtkCellData.h"
#include "vtkDataArraySelection.h"
#include "vtkDataSetAttributes.h"
#include "vtkErrorCode.h"
#include "vtkFieldData.h"
#include "vtkFloatArray.h"
#include "vtkIdTypeArray.h"
#include "vtkInformation.h"
#include "vtkInformationVector.h"
#include "vtkIntArray.h"
#include "vtkObjectFactory.h"
#include "vtkPointData.h"
#include "vtkStreamingDemandDrivenPipeline.h"
#include "vtkUnstructuredGrid.h"

#include <sys/time.h>
#include <algorithm>
#include <vector>
#include <string>
#include <hdf5.h>
using namespace std;

#include "vtkMPICommunicator.h"
#include "vtkMPIController.h"
#include "vtkMPI.h"
#include <set>
#include <sstream>
hid_t    file_id;

vtkStandardNewMacro(vtkSalvusHDF5Reader);

int vtkSalvusHDF5Reader::CanReadFile(const char* fname )
{
  int ret = 0;
  if (! fname )
    return ret;
  cerr << __LINE__ << ": CanReadFile(" << fname << ")\n";
  hid_t f_id = H5Fopen(fname, H5F_ACC_RDONLY, H5P_DEFAULT);
  hid_t root_id = H5Gopen(f_id, "/", H5P_DEFAULT);
  if(H5Lexists(root_id, "/volume", H5P_DEFAULT))
    ret = 1;

  H5Gclose(root_id);
  H5Fclose(f_id);
  return ret;
}

vtkSalvusHDF5Reader::vtkSalvusHDF5Reader()
{
  this->FileName = nullptr; 
  this->DebugOff();
  this->SetNumberOfInputPorts(0);
  this->SetNumberOfOutputPorts(1);
  this->ELASTIC_PointDataArraySelection = vtkDataArraySelection::New();
  this->ACOUSTIC_PointDataArraySelection = vtkDataArraySelection::New();
  this->NumberOfTimeSteps = 0;
  this->TimeStep = 0;
  this->ActualTimeStep = 0;
  this->TimeStepTolerance = 1E-6;
  
  this->varnames[0] = {"stress_xx", "stress_yy", "stress_zz", "stress_yz", "stress_xz", "stress_xy"};
  this->varnames[1] = {"phi_tt"};
}
 
vtkSalvusHDF5Reader::~vtkSalvusHDF5Reader()
{
  vtkDebugMacro(<< "cleaning up inside destructor");
  if (this->FileName)
    delete [] this->FileName;
  this->ELASTIC_PointDataArraySelection->Delete();
  this->ELASTIC_PointDataArraySelection = nullptr;
  this->ACOUSTIC_PointDataArraySelection->Delete();
  this->ACOUSTIC_PointDataArraySelection = nullptr;
}

int vtkSalvusHDF5Reader::RequestInformation(
                         vtkInformation *vtkNotUsed(request),
                         vtkInformationVector **vtkNotUsed(inputVector),
                         vtkInformationVector* outputVector)
{
  file_id = H5Fopen(this->FileName, H5F_ACC_RDONLY, H5P_DEFAULT);
  hid_t root_id, mesh_id, coords_id;
  hid_t filespace0, filespace1, attr1;
  hsize_t dimsf[3];
  herr_t  status;

  vtkInformation* outInfo = outputVector->GetInformationObject(0);
  outInfo->Set(CAN_HANDLE_PIECE_REQUEST(), 1);

  outInfo->Set(vtkDataObject::DATA_NUMBER_OF_GHOST_LEVELS(), 0);

  root_id = H5Gopen(file_id, "/", H5P_DEFAULT);

  if(this->ModelName == 0)
  {
    mesh_id = H5Dopen(root_id, "connectivity_ELASTIC", H5P_DEFAULT);
    coords_id = H5Dopen(root_id, "coordinates_ELASTIC", H5P_DEFAULT);
  }
  else
  {
    mesh_id = H5Dopen(root_id, "connectivity_ACOUSTIC", H5P_DEFAULT);
    coords_id = H5Dopen(root_id, "coordinates_ACOUSTIC", H5P_DEFAULT);
  }
  filespace0 = H5Dget_space(mesh_id);
  H5Sget_simple_extent_dims(filespace0, dimsf, NULL);
  this->NbCells = dimsf[0];
  H5Sclose(filespace0);
  H5Dclose(mesh_id);

  filespace1 = H5Dget_space(coords_id);
  H5Sget_simple_extent_dims(filespace1, dimsf, NULL);
  this->NbNodes = dimsf[0] * dimsf[1];
  H5Sclose(filespace1);
  H5Dclose(coords_id);

  int MeshSizes[2] = {this->NbNodes, this->NbCells};
  if(H5Lexists(root_id, "volume", H5P_DEFAULT))
  {
    hid_t volume_id = H5Gopen(root_id, "volume", H5P_DEFAULT);
    if(volume_id >= 0)
    {
      double dt, sampling_rate, t_start;
      attr1 = H5Aopen(volume_id, "sampling_rate_in_hertz", H5P_DEFAULT);
      status = H5Aread(attr1, H5T_NATIVE_DOUBLE, &sampling_rate);
      dt = 1.0 / sampling_rate;
      status = H5Aclose(attr1);
      
      attr1 = H5Aopen(volume_id, "start_time_in_seconds", H5P_DEFAULT);
      status = H5Aread(attr1, H5T_NATIVE_DOUBLE, &t_start);
      status = H5Aclose(attr1);

      if(H5Lexists(volume_id, "stress", H5P_DEFAULT))
      {
        hid_t stress_id = H5Dopen(volume_id, "stress", H5P_DEFAULT);
        filespace0 = H5Dget_space(stress_id);
        H5Sget_simple_extent_dims(filespace0, dimsf, NULL);
        H5Sclose(filespace0);
        this->NumberOfTimeSteps = dimsf[0];
        H5Dclose(stress_id);
      }
      else if(H5Lexists(volume_id, "phi_tt", H5P_DEFAULT))
      {
        hid_t phi_tt_id = H5Dopen(volume_id, "phi_tt", H5P_DEFAULT);
        filespace0 = H5Dget_space(phi_tt_id);
        H5Sget_simple_extent_dims(filespace0, dimsf, NULL);
        H5Sclose(filespace0);
        this->NumberOfTimeSteps = dimsf[0];
        H5Dclose(phi_tt_id);
      }
      for(auto varn : this->varnames[0])
        this->ELASTIC_PointDataArraySelection->AddArray(varn.c_str());
      for(auto varn : this->varnames[1])
        this->ACOUSTIC_PointDataArraySelection->AddArray(varn.c_str());

      this->TimeStepValues.assign(this->NumberOfTimeSteps, 0.0);
      for(int i=0; i < NumberOfTimeSteps; i++)
        this->TimeStepValues[i] = t_start + i * (dt);
      
      double timeRange[2];
      timeRange[0] = this->TimeStepValues.front();
      timeRange[1] = this->TimeStepValues.back();

      outInfo->Set(vtkStreamingDemandDrivenPipeline::TIME_RANGE(), timeRange, 2);
      outInfo->Set(vtkStreamingDemandDrivenPipeline::TIME_STEPS(), this->TimeStepValues.data(),
                   static_cast<int>(this->TimeStepValues.size()));
    }
    H5Gclose(volume_id);
    }

  H5Gclose(root_id);
  H5Fclose(file_id);
  return 1;
}

int vtkSalvusHDF5Reader::RequestData(
                vtkInformation* vtkNotUsed(request),
                vtkInformationVector** vtkNotUsed(inputVector),
                vtkInformationVector* outputVector)
{
  hid_t root_id, mesh_id, coords_id, volume_id, data_id=0;
  hid_t filespace;
  hsize_t dimsf[3];
  herr_t   status;
  struct timeval tv0, tv1, res;

  vtkDebugMacro( << "RequestData(BEGIN)");
  vtkInformation* outInfo = outputVector->GetInformationObject(0);
  vtkDataObject* doOutput = outInfo->Get(vtkDataObject::DATA_OBJECT());
  vtkUnstructuredGrid* output = vtkUnstructuredGrid::SafeDownCast(doOutput);

  int piece = outInfo->Get(vtkStreamingDemandDrivenPipeline::UPDATE_PIECE_NUMBER());
  int numPieces = outInfo->Get(vtkStreamingDemandDrivenPipeline::UPDATE_NUMBER_OF_PIECES());

#ifdef PARALLEL_DEBUG
  std::ostringstream fname;
#ifdef __CRAYXC
  fname << "/scratch/snx3000/jfavre/out." << piece << ".txt" << ends;
#elseif defined(__CRAY_X86_ROME)
  fname << "/iopsstor/scratch/cscs/jfavre/out." << piece << ".txt" << ends;
#else
  fname << "/tmp/out." << piece << ".txt" << ends;
#endif
  ofstream errs;
  errs.open(fname.str().c_str(),ios::ate);
  errs << "piece " << piece << " out of " << numPieces << endl;
#endif

  this->ActualTimeStep = 0;
  double requestedTimeValue = 0.0;
  if (outInfo->Has(vtkStreamingDemandDrivenPipeline::UPDATE_TIME_STEP()))
  {
    requestedTimeValue = outInfo->Get(vtkStreamingDemandDrivenPipeline::UPDATE_TIME_STEP());

    for (int i=0; i< this->NumberOfTimeSteps; i++)
    {
      if (fabs(requestedTimeValue - this->TimeStepValues[i]) < this->TimeStepTolerance)
      {
	this->ActualTimeStep = i;
	break;
      }
    }
    output->GetInformation()->Set(vtkDataObject::DATA_TIME_STEP(), requestedTimeValue);
  }
  cout << "requestedTimeValue "  << requestedTimeValue << endl;
  cout << "this->ActualTimeStep "  << this->ActualTimeStep << endl;

  vtkDebugMacro(<< "getting piece " << piece << " out of " << numPieces << " pieces");
  if (!this->FileName)
  {
    vtkErrorMacro(<< "error reading header specified!");
    return 0;
  }
  size_t size;

  file_id = H5Fopen(this->FileName, H5F_ACC_RDONLY, H5P_DEFAULT);
  root_id = H5Gopen(file_id, "/", H5P_DEFAULT);
  volume_id = H5Gopen(root_id, "volume", H5P_DEFAULT);
  if(this->ModelName == ELASTIC)
  {
    mesh_id   = H5Dopen(root_id, "connectivity_ELASTIC", H5P_DEFAULT);
    coords_id = H5Dopen(root_id, "coordinates_ELASTIC", H5P_DEFAULT);
    data_id   = H5Dopen(volume_id, "stress", H5P_DEFAULT);
  }
  else
  {
    mesh_id   = H5Dopen(root_id, "connectivity_ACOUSTIC", H5P_DEFAULT);
    coords_id = H5Dopen(root_id, "coordinates_ACOUSTIC", H5P_DEFAULT);
    data_id   = H5Dopen(volume_id, "phi_tt", H5P_DEFAULT);
  }
#ifdef PARALLEL_DEBUG
  errs <<"this->NbNodes = " << this->NbNodes << ", this->NbCells = " << this->NbCells << std::endl;
#endif

// here we allocate the final list necessary for VTK. It includes an extra
// integer for every hexahedra to say that the next cell contains 8 nodes.
// to avoid allocating two lists and making transfers from one to the other
// we pre-allocate for 9 * NbCells.
// We use HDF5's memory space to conveniently place the Nx8 array read from disk
// in the Nx9 matrix, leaving the left-most column empty to add the value 8.
  long MyNumber_of_Cells, MyNumber_of_Nodes;
  long load;
  vtkIdType *vtkfinal_id_list, minId, maxId, *destptr;
  hsize_t count[4], offset[4];

  hid_t memspace, dataspace;

  if(numPieces == 1)
  {
    load = MyNumber_of_Cells = this->NbCells;
  }
  else
  {
    load = this->NbCells / numPieces;
    if (piece < (numPieces-1))
    {
      MyNumber_of_Cells = load;
    }
    else
    {
      MyNumber_of_Cells = this->NbCells - (numPieces-1) * load;
    }
  }
#ifdef PARALLEL_DEBUG
  errs << "CPU " << piece << ": allocating " << MyNumber_of_Cells  << " list of elements of size " << "*9*" << sizeof(vtkIdType) << " bytes = " << MyNumber_of_Cells * 9 *sizeof(vtkIdType) << " bytes\n";
#endif
  vtkIdTypeArray *vtklistcells = vtkIdTypeArray::New();
  vtklistcells->SetNumberOfValues(MyNumber_of_Cells * (8 + 1));
  vtkfinal_id_list = vtklistcells->GetPointer(0);

  count[0] = MyNumber_of_Cells;
  count[1] = 8 + 1;
  memspace = H5Screate_simple(2, count, NULL);

  offset[0] = 0;
  offset[1] = 1;
  count[0] = MyNumber_of_Cells;
  count[1] = 8;
  H5Sselect_hyperslab (memspace, H5S_SELECT_SET, offset, NULL, count, NULL);
  hsize_t     dims_out[2];
  dataspace = H5Dget_space(mesh_id);
  int ndims  = H5Sget_simple_extent_ndims(dataspace);
  H5Sget_simple_extent_dims(dataspace, dims_out, NULL);
  //printf("ndims %d, dimensions %lu x %lu \n", ndims, (unsigned long)(dims_out[0]), (unsigned long)(dims_out[1]));
  count[0] = MyNumber_of_Cells;
  count[1] = 8;
  offset[0] = piece * load;
  offset[1] = 0;
  //cerr <<  "CPU " << piece << ": count = " << count[0] << " x " <<  count[1] << endl;
  //cerr <<  "CPU " << piece << ": offset = " << offset[0] << " x " <<  offset[1] << endl;
  //dataspace = H5Dget_space(mesh_id);
  H5Sselect_hyperslab(dataspace, H5S_SELECT_SET, offset, NULL, count, NULL);

  hid_t plist_xfer = H5Pcreate(H5P_DATASET_XFER);
  status = H5Pset_buffer(plist_xfer, (hsize_t)1*1024*1024, NULL, NULL);
  // this is the default value
  if (sizeof(vtkIdType) == H5Tget_size(H5T_NATIVE_INT))
  {
    H5Dread(mesh_id, H5T_NATIVE_INT, memspace, dataspace,
            plist_xfer,
            vtkfinal_id_list);
  }
  else if (sizeof(vtkIdType) == H5Tget_size(H5T_NATIVE_LONG))
  {
    H5Dread(mesh_id, H5T_NATIVE_LONG, memspace, dataspace,
            plist_xfer,
            vtkfinal_id_list);
  }
  else {
    cerr << "type&size error while reading element connectivity\n";
  }
  H5Pclose (plist_xfer);

  H5Dclose(mesh_id);
  H5Sclose(memspace);

  minId = vtkIdTypeArray::GetDataTypeValueMax();
  maxId = -1;
  destptr = vtklistcells->GetPointer(0);

  if(numPieces == 1) // serial job, do not renumber
  {
    for(int i= 0 ; i< MyNumber_of_Cells; i++)
    {
      *destptr++ = 8;
      destptr += 8; // skip 8 positions
    }
    MyNumber_of_Nodes = this->NbNodes; minId = 0;
  }
  else
  {
    //vtkIdTypeArray* originalPtIds = vtkIdTypeArray::New();
    //originalPtIds->SetNumberOfComponents(1);
    //originalPtIds->SetName("GlobalNodeIds");

    for(int i= 0 ; i< MyNumber_of_Cells; i++)
    {
      *destptr++ = 8;
      for(int j = 0; j < 8; j++)
      {
        if((*destptr) > maxId) maxId = (*destptr);
        if((*destptr) < minId) minId = (*destptr);
        destptr++;
      }
    }
    destptr = vtklistcells->GetPointer(0);
    for(int i= 0 ; i< MyNumber_of_Cells; i++)
    {
      destptr++; // skip the first which contains the value 8
      for(int j = 0; j < 8; j++)
      {
        (*destptr) -= minId; // all nodes now start from 0
        destptr++;
      }
    }

    MyNumber_of_Nodes = maxId - minId + 1;
    //originalPtIds->SetNumberOfTuples(MyNumber_of_Nodes);
#ifdef PARALLEL_DEBUG
  //errs << __LINE__ << ": allocating " << MyNumber_of_Nodes << " OriginalPointIds of size " <<  sizeof(vtkIdType) << " bytes = " << MyNumber_of_Nodes *sizeof(vtkIdType) << " bytes\n";
#endif
    //for(int i= 0 ; i< MyNumber_of_Nodes; i++)
    //{
      //originalPtIds->SetTuple1(i, minId+i);
    //}
    //output->GetPointData()->SetGlobalIds(originalPtIds);
    //originalPtIds->FastDelete();
  }


  vtkCellArray *cells = vtkCellArray::New();

  cells->SetCells(MyNumber_of_Cells, vtklistcells);
  vtklistcells->FastDelete();

  int *types = new int[MyNumber_of_Cells];
#ifdef PARALLEL_DEBUG
  errs << __LINE__<< ": allocating " << MyNumber_of_Cells << " celltypes of size " <<  sizeof(int) << " bytes = " << MyNumber_of_Cells *sizeof(int) << " bytes\n";
#endif
  for(int i=0; i < MyNumber_of_Cells; i++)
  {
    types[i] = VTK_HEXAHEDRON;
  }

  output->SetCells(types, cells);
  cells->FastDelete();
  delete [] types;
  this->UpdateProgress(0.50);
  float *coords0 = nullptr;
  vtkFloatArray *coords = vtkFloatArray::New(); // destination array
  coords->SetNumberOfComponents(3);
  coords->SetNumberOfTuples(MyNumber_of_Nodes);
  if(numPieces > 1)
  {
    coords0 = new float[3 * this->NbNodes]; // temporary storage for the *full* list
  }
#ifdef PARALLEL_DEBUG
  errs << __LINE__ << ": allocating " << MyNumber_of_Nodes  << " coordinates of size " <<  3*sizeof(float) << " bytes = " << MyNumber_of_Nodes * 3 *sizeof(float) << " bytes\n";
#endif

  count[0] = this->NbNodes;
  count[1] = 3;
  memspace = H5Screate_simple(2, count, NULL);

  offset[0] = 0;
  offset[1] = 0;
  count[0] = this->NbNodes;
  count[1] = 3;
  H5Sselect_hyperslab (memspace, H5S_SELECT_SET, offset, NULL, count, NULL);

  count[0] = this->NbNodes/125;
  count[1] = 125;
  count[2] = 3;
  offset[0] = 0;
  offset[1] = 0;
  offset[2] = 0;

  dataspace = H5Dget_space(coords_id);
  H5Sselect_hyperslab(dataspace, H5S_SELECT_SET, offset, NULL, count, NULL);

  if(numPieces == 1)
    {
    status = H5Dread(coords_id, H5T_NATIVE_FLOAT, memspace, dataspace, H5P_DEFAULT,
            static_cast<vtkFloatArray *>(coords)->GetPointer(0));
    }
  else
    {
    status = H5Dread(coords_id, H5T_NATIVE_FLOAT, memspace, dataspace, H5P_DEFAULT, coords0);

    // void *memcpy(void *dest, const void *src, size_t n);
    memcpy(static_cast<vtkFloatArray *>(coords)->GetPointer(0),
           &coords0[minId*3], 
           sizeof(float) * 3 * MyNumber_of_Nodes);
    delete [] coords0;
    }
  H5Dclose(coords_id);
  H5Sclose(memspace);
  vtkPoints *points = vtkPoints::New();
  points->SetData(coords);
  coords->FastDelete();
  output->SetPoints(points);
  points->FastDelete();

  this->UpdateProgress(0.70);

  // following code will read either ELASTIC or ACOUSTIC data depending on how variable this->ModelName is set
  this->Load_Variables(output, numPieces, MyNumber_of_Nodes, minId, data_id);

  H5Dclose(data_id);
  H5Gclose(volume_id);
  H5Gclose(root_id);
  H5Fclose(file_id);
  this->UpdateProgress(1.0);
#ifdef PARALLEL_DEBUG
  errs << "RequestData(END)\n";
  errs.close();
#endif
  //cleaner = vtkStaticCleanUnstructuredGrid()
  //cleaner.SetInputData(tmpOutput)
  //cleaner.Update()

  //output.ShallowCopy(cleaner.GetOutput())
  return 1;
}

bool vtkSalvusHDF5Reader::Is_Variable_Enabled(const char* vname)
{
  if(this->ModelName == ELASTIC)
    return GetPointArrayStatus(vname);
  else
    return Get_Acoustic_PointArrayStatus(vname);
}

void vtkSalvusHDF5Reader::Load_Variables(vtkUnstructuredGrid* output, const int numPieces, const int MyNumber_of_Nodes, const int minId, long int dset_id)
{
  hsize_t count[4], offset[4];
  hid_t memspace, dataspace, data_id = static_cast<hid_t >(dset_id);
  herr_t status;
  
  float *data0 = nullptr;
  
  if(numPieces > 1)
  {
    data0 = new float[this->NbNodes]; // temporary storage
  }
  for(int i=0; i < this->varnames[this->ModelName].size(); i++)
  {
    const char *vname = this->varnames[this->ModelName][i].c_str();
    if(this->Is_Variable_Enabled(vname)) // is variable enabled in the ParaView GUI
    {
      vtkFloatArray *data = vtkFloatArray::New(); // destination array
      data->SetNumberOfComponents(1);
      data->SetNumberOfTuples(MyNumber_of_Nodes);
      data->SetName(vname);
    
      count[0] = this->NbNodes;
      count[1] = 1;
      memspace = H5Screate_simple(2, count, NULL);

      offset[0] = 0;
      offset[1] = 0;
      count[0] = this->NbNodes;
      count[1] = 1;
      H5Sselect_hyperslab (memspace, H5S_SELECT_SET, offset, NULL, count, NULL);

      count[0] = 1; // timestep slice
      count[1] = this->NbNodes/125;
      count[2] = 1;
      count[3] = 125;
      offset[0] = this->ActualTimeStep;
      offset[1] = 0;
      offset[2] = i;
      offset[3] = 0;
      dataspace = H5Dget_space(data_id);
      H5Sselect_hyperslab(dataspace, H5S_SELECT_SET, offset, NULL, count, NULL);

      if(numPieces == 1)
      {
        status = H5Dread(data_id, H5T_NATIVE_FLOAT, memspace, dataspace, H5P_DEFAULT,
            static_cast<vtkFloatArray *>(data)->GetPointer(0));
      }
      else
      {
        status = H5Dread(data_id, H5T_NATIVE_FLOAT, memspace, dataspace, H5P_DEFAULT, data0);
          // void *memcpy(void *dest, const void *src, size_t n);
        memcpy(static_cast<vtkFloatArray *>(data)->GetPointer(0),
                 &data0[minId], 
                 sizeof(float) * MyNumber_of_Nodes);
      }
      output->GetPointData()->AddArray(data);
      data->FastDelete();
    }
  }
  if(numPieces > 1)
  {
    delete [] data0;
  }
}

void vtkSalvusHDF5Reader::EnablePointArray(const char* name)
{
  this->SetPointArrayStatus(name, 1);
}

void vtkSalvusHDF5Reader::DisablePointArray(const char* name)
{
  this->SetPointArrayStatus(name, 0);
}

void vtkSalvusHDF5Reader::EnableAllPointArrays()
{
  this->ELASTIC_PointDataArraySelection->EnableAllArrays();
}

void vtkSalvusHDF5Reader::DisableAllPointArrays()
{
  this->ELASTIC_PointDataArraySelection->DisableAllArrays();
}

const char* vtkSalvusHDF5Reader::GetPointArrayName(int index)
{
  return this->ELASTIC_PointDataArraySelection->GetArrayName(index);
}

int vtkSalvusHDF5Reader::GetPointArrayStatus(const char* name)
{
  return this->ELASTIC_PointDataArraySelection->ArrayIsEnabled(name);
}

void vtkSalvusHDF5Reader::SetPointArrayStatus(const char* name, int status)
{
  if(status)
    {
    this->ELASTIC_PointDataArraySelection->EnableArray(name);
    }
  else
    {
    this->ELASTIC_PointDataArraySelection->DisableArray(name);
    }
}

int vtkSalvusHDF5Reader::GetNumberOfPointArrays()
{
  return this->ELASTIC_PointDataArraySelection->GetNumberOfArrays();
}

/*******************************************************/
void vtkSalvusHDF5Reader::Enable_Acoustic_PointArray(const char* name)
{
  this->Set_Acoustic_PointArrayStatus(name, 1);
}

void vtkSalvusHDF5Reader::Disable_Acoustic_PointArray(const char* name)
{
  this->Set_Acoustic_PointArrayStatus(name, 0);
}

void vtkSalvusHDF5Reader::EnableAll_Acoustic_PointArrays()
{
  this->ACOUSTIC_PointDataArraySelection->EnableAllArrays();
}

void vtkSalvusHDF5Reader::DisableAll_Acoustic_PointArrays()
{
  this->ACOUSTIC_PointDataArraySelection->DisableAllArrays();
}

const char* vtkSalvusHDF5Reader::Get_Acoustic_PointArrayName(int index)
{
  return this->ACOUSTIC_PointDataArraySelection->GetArrayName(index);
}

int vtkSalvusHDF5Reader::Get_Acoustic_PointArrayStatus(const char* name)
{
  return this->ACOUSTIC_PointDataArraySelection->ArrayIsEnabled(name);
}

void vtkSalvusHDF5Reader::Set_Acoustic_PointArrayStatus(const char* name, int status)
{
  if(status)
    {
    this->ACOUSTIC_PointDataArraySelection->EnableArray(name);
    }
  else
    {
    this->ACOUSTIC_PointDataArraySelection->DisableArray(name);
    }
}

int vtkSalvusHDF5Reader::GetNumberOf_Acoustic_PointArrays()
{
  return this->ACOUSTIC_PointDataArraySelection->GetNumberOfArrays();
}

/*
/                        Group
/connectivity_ACOUSTIC   Dataset {364611648, 8}
/connectivity_ELASTIC    Dataset {70320320, 8}
/coordinates_ACOUSTIC    Dataset {5697057, 125, 3}
/coordinates_ELASTIC     Dataset {1098755, 125, 3}
/partitioning            Group
/partitioning/globalIdSizes Dataset {1200}
/partitioning/globalIds  Dataset {6795812}
/partitioning/points     Dataset {6795812}
/partitioning/rankSizes  Dataset {1200}
/partitioning/ranks      Dataset {22564}
/partitioning/sizes      Dataset {22564}
/volume                  Group
/volume/phi_tt           Dataset {11, 5697057, 1, 128}
/volume/stress           Dataset {11, 1098755, 6, 128}
*/
