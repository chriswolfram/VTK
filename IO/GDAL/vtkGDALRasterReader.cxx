/*=========================================================================

  Program:   Visualization Toolkit
  Module:    vtkGDALRasterReader.cxx

  Copyright (c) Ken Martin, Will Schroeder, Bill Lorensen
  All rights reserved.
  See Copyright.txt or http://www.kitware.com/Copyright.htm for details.

     This software is distributed WITHOUT ANY WARRANTY; without even
     the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
     PURPOSE.  See the above copyright notice for more information.

=========================================================================*/

#include "vtkGDALRasterReader.h"

// VTK includes
#include "vtkCellArray.h"
#include "vtkCellData.h"
#include "vtkDoubleArray.h"
#include "vtkFloatArray.h"
#include "vtkGDAL.h"
#include "vtkInformationVector.h"
#include "vtkInformation.h"
#include "vtkIntArray.h"
#include "vtkLookupTable.h"
#include "vtkMath.h"
#include "vtkObjectFactory.h"
#include "vtkPointData.h"
#include "vtkPoints.h"
#include "vtkShortArray.h"
#include "vtkSmartPointer.h"
#include "vtkStreamingDemandDrivenPipeline.h"
#include "vtkStringArray.h"
#include "vtkUniformGrid.h"
#include "vtkUnsignedCharArray.h"
#include "vtkUnsignedIntArray.h"
#include "vtkUnsignedShortArray.h"

// GDAL includes
#include <gdal_priv.h>
#include <ogr_spatialref.h>

// C/C++ includes
#include <cassert>
#include <iostream>
#include <sstream>
#include <vector>

vtkStandardNewMacro(vtkGDALRasterReader);

//-----------------------------------------------------------------------------
namespace
{
  double Min(double val1, double val2)
  {
    return ((val1 < val2) ? val1 : val2);
  }

  double Max(double val1, double val2)
  {
    return ((val1 > val2) ? val1 : val2);
  }
}

//-----------------------------------------------------------------------------
class vtkGDALRasterReader::vtkGDALRasterReaderInternal
{
public:
   vtkGDALRasterReaderInternal(vtkGDALRasterReader* reader);
  ~vtkGDALRasterReaderInternal();

  void ReadMetaData(const std::string& fileName);
  void ReadData(const std::string& fileName);

  template <typename VTK_TYPE, typename RAW_TYPE> void GenericReadData();
  void ReleaseData();

  template <typename VTK_TYPE, typename RAW_TYPE>
  void Convert(std::vector<RAW_TYPE>& rawUniformGridData,
               int targetWidth, int targetHeight, const std::vector<int>& groupIndex,
               const char* name,
               bool flipX, bool flipY);

  bool GetGeoCornerPoint(GDALDataset* dataset,
                         double x, double y, double* out) const;

  const double* GetGeoCornerPoints();

  void ReadColorTable(
    GDALRasterBand *rasterBand, vtkLookupTable *colorTable) const;

  int NumberOfBands;
  int NumberOfBytesPerPixel;

  int SourceOffset[2];
  int SourceDimensions[2];

  std::string PrevReadFileName;

  GDALDataset* GDALData;
  GDALDataType TargetDataType;

  // Bad corner point
  double BadCornerPoint;

  // Upper left, lower left, upper right, lower right
  double CornerPoints[8];

  std::vector<int> HasNoDataValue;
  std::vector<double> NoDataValue;
  vtkIdType NumberOfCells;

  vtkSmartPointer<vtkUniformGrid> UniformGridData;
  vtkGDALRasterReader* Reader;
};

//-----------------------------------------------------------------------------
vtkGDALRasterReader::vtkGDALRasterReaderInternal::vtkGDALRasterReaderInternal(
  vtkGDALRasterReader* reader) :
  NumberOfBands(0),
  NumberOfBytesPerPixel(0),
  GDALData(0),
  TargetDataType(GDT_Byte),
  BadCornerPoint(-1),
  NumberOfCells(0),
  UniformGridData(0),
  Reader(reader)
{
  this->SourceOffset[0] = 0;
  this->SourceOffset[1] = 0;
  this->SourceDimensions[0] = 0;
  this->SourceDimensions[1] = 0;

  for (int i = 0; i < 8; ++i)
  {
    this->CornerPoints[i] = this->BadCornerPoint;
  }

  // Enable all the drivers.
  GDALAllRegister();
}

//-----------------------------------------------------------------------------
vtkGDALRasterReader::vtkGDALRasterReaderInternal::~vtkGDALRasterReaderInternal()
{
  this->ReleaseData();
}

//-----------------------------------------------------------------------------
void vtkGDALRasterReader::vtkGDALRasterReaderInternal::ReadMetaData(
  const std::string& fileName)
{
  if (fileName.compare(this->PrevReadFileName) == 0)
  {
    return;
  }

  // Free up the last read data, if any.
  this->ReleaseData();

  this->GDALData = static_cast<GDALDataset*>(
                     GDALOpen(fileName.c_str(), GA_ReadOnly));

  if (this->GDALData == nullptr)
  {
    std::cout << "NO GDALData loaded for file "
              << fileName << std::endl;
  }
  else
  {
    this->PrevReadFileName = fileName;
    this->NumberOfBands = this->GDALData->GetRasterCount();
    this->HasNoDataValue.resize(this->NumberOfBands, 0);
    this->NoDataValue.resize(this->NumberOfBands, 0);

    // Clear last read metadata
    this->Reader->MetaData.clear();

    this->Reader->RasterDimensions[0] = this->GDALData->GetRasterXSize();
    this->Reader->RasterDimensions[1] = this->GDALData->GetRasterYSize();

    GDALDriverH driver = GDALGetDatasetDriver(this->GDALData);
    this->Reader->DriverShortName = GDALGetDriverShortName(driver);
    this->Reader->DriverLongName = GDALGetDriverLongName(driver);

    char** papszMetaData = GDALGetMetadata(this->GDALData, nullptr);
    if (CSLCount(papszMetaData) > 0)
    {
      for (int i = 0; papszMetaData[i] != nullptr; ++i)
      {
        this->Reader->MetaData.push_back(papszMetaData[i]);
      }
    }
  }
}

//-----------------------------------------------------------------------------
void vtkGDALRasterReader::vtkGDALRasterReaderInternal::ReadData(
  const std::string& fileName)
{
  // If data is not initialized by now, it means that we were unable to read
  // the file.
  if (!this->GDALData)
  {
    std::cerr << "Failed to read: " << fileName << std::endl;
    return;
  }

  // all bands have the same data type (true for most drivers)
  // https://lists.osgeo.org/pipermail/gdal-dev/2016-September/045166.html
  GDALRasterBand* rasterBand = this->GDALData->GetRasterBand(1);
  if (this->NumberOfBytesPerPixel == 0)
  {
    this->TargetDataType = rasterBand->GetRasterDataType();
    switch (this->TargetDataType)
    {
    case (GDT_Byte): this->NumberOfBytesPerPixel = 1; break;
    case (GDT_UInt16): this->NumberOfBytesPerPixel = 2; break;
    case (GDT_Int16): this->NumberOfBytesPerPixel = 2; break;
    case (GDT_UInt32): this->NumberOfBytesPerPixel = 4; break;
    case (GDT_Int32): this->NumberOfBytesPerPixel = 4; break;
    case (GDT_Float32): this->NumberOfBytesPerPixel = 4; break;
    case (GDT_Float64): this->NumberOfBytesPerPixel = 8; break;
    default: this->NumberOfBytesPerPixel = 0; break;
    }
  }

  // Initialize
  this->UniformGridData = vtkSmartPointer<vtkUniformGrid>::New();
  this->NumberOfCells = 0;

  switch (this->TargetDataType)
  {
    case (GDT_UInt16):
    {
      this->Reader->SetDataScalarTypeToUnsignedShort();
      this->GenericReadData<vtkUnsignedShortArray, unsigned short>();
      break;
    }
    case (GDT_Int16):
    {
      this->Reader->SetDataScalarTypeToShort();
      this->GenericReadData<vtkShortArray, short>();
      break;
    }
    case (GDT_UInt32):
    {
      this->Reader->SetDataScalarTypeToUnsignedInt();
      this->GenericReadData<vtkUnsignedIntArray, unsigned int>();
      break;
    }
    case (GDT_Int32):
    {
      this->Reader->SetDataScalarTypeToInt();
      this->GenericReadData<vtkIntArray, int>();
      break;
    }
    case (GDT_Float32):
    {
      this->Reader->SetDataScalarTypeToFloat();
      this->GenericReadData<vtkFloatArray, float>();
      break;
    }
    case (GDT_Float64):
    {
      this->Reader->SetDataScalarTypeToDouble();
      this->GenericReadData<vtkDoubleArray, double>();
      break;
    }
    case (GDT_Byte):
    default:
    {
      this->Reader->SetDataScalarTypeToUnsignedChar();
      this->GenericReadData<vtkUnsignedCharArray, unsigned char>();
      break;
    }
  }
}

//-----------------------------------------------------------------------------
template <typename VTK_TYPE, typename RAW_TYPE>
void vtkGDALRasterReader::vtkGDALRasterReaderInternal::GenericReadData()
{
  // Pixel data.
  std::vector<RAW_TYPE> rawUniformGridData;

  // Color table
  vtkSmartPointer<vtkLookupTable> colorTable =
    vtkSmartPointer<vtkLookupTable>::New();

  // Possible bands
  GDALRasterBand* redBand = nullptr;
  int redIndex = 0;
  GDALRasterBand* greenBand = nullptr;
  int greenIndex = 0;
  GDALRasterBand* blueBand = nullptr;
  int blueIndex = 0;
  GDALRasterBand* alphaBand = nullptr;
  int alphaIndex = 0;
  GDALRasterBand* grayBand = nullptr;
  int grayIndex = 0;
  GDALRasterBand* paletteBand = nullptr;
  int paletteIndex = 0;
  std::vector<GDALRasterBand*> otherBands;
  std::vector<int> otherIndex;
  std::vector<std::vector<RAW_TYPE>> otherBandsData;

  for (int i = 1; i <= this->NumberOfBands; ++i)
  {
    GDALRasterBand* rasterBand = this->GDALData->GetRasterBand(i);
    this->HasNoDataValue[i - 1] = 0;
    this->NoDataValue[i - 1] = rasterBand->GetNoDataValue(&this->HasNoDataValue[i - 1]);
    otherBands.push_back(rasterBand);
    otherIndex.push_back(i);

    if ((rasterBand->GetColorInterpretation() == GCI_RedBand ||
         rasterBand->GetColorInterpretation() == GCI_YCbCr_YBand) && redIndex == 0)
    {
      redBand = rasterBand;
      redIndex = i;
    }
    else if ((rasterBand->GetColorInterpretation() == GCI_GreenBand ||
              rasterBand->GetColorInterpretation() == GCI_YCbCr_CbBand) && greenIndex == 0)
    {
      greenBand = rasterBand;
      greenIndex = i;
    }
    else if ((rasterBand->GetColorInterpretation() == GCI_BlueBand ||
              rasterBand->GetColorInterpretation() == GCI_YCbCr_CrBand) && blueIndex == 0)
    {
      blueBand = rasterBand;
      blueIndex = i;
    }
    else if (rasterBand->GetColorInterpretation() == GCI_AlphaBand && alphaIndex == 0)
    {
      alphaBand = rasterBand;
      alphaIndex = i;
    }
    else if (rasterBand->GetColorInterpretation() == GCI_GrayIndex && grayIndex == 0)
    {
      grayBand = rasterBand;
      grayIndex = i;
    }
    else if (rasterBand->GetColorInterpretation() == GCI_PaletteIndex && paletteIndex == 0)
    {
      paletteBand = rasterBand;
      paletteIndex = i;
    }
    else
    {
      // GCI_Undefined or duplicates for colors or gray
    }
  }

  const int& destWidth = this->Reader->TargetDimensions[0];
  const int& destHeight = this->Reader->TargetDimensions[1];

  // GDAL top left is at 0,0
  const int& windowX = this->SourceOffset[0];
  const int& windowY = this->SourceOffset[1];
  const int& windowWidth = this->SourceDimensions[0];
  const int& windowHeight = this->SourceDimensions[1];

  const int& pixelSpace = this->NumberOfBytesPerPixel;
  const int lineSpace = destWidth * pixelSpace;
  const int bandSpace = destWidth * destHeight * this->NumberOfBytesPerPixel;
  CPLErr err;

  std::vector<int> groupIndex;
  // TODO: Support other band types
  if (redBand && greenBand && blueBand)
  {
    otherBands[redIndex - 1] = nullptr;
    otherIndex[redIndex - 1] = 0;
    groupIndex.push_back(redIndex);
    otherBands[greenIndex - 1] = nullptr;
    otherIndex[greenIndex - 1] = 0;
    groupIndex.push_back(greenIndex);
    otherBands[blueIndex - 1] = nullptr;
    otherIndex[blueIndex - 1] = 0;
    groupIndex.push_back(blueIndex);
    if (alphaBand)
    {
      otherBands[alphaIndex - 1] = nullptr;
      otherIndex[alphaIndex - 1] = 0;
      groupIndex.push_back(alphaIndex);
      this->Reader->SetNumberOfScalarComponents(4);
      rawUniformGridData.resize(4 * destWidth * destHeight * pixelSpace);

      err = redBand->RasterIO(
              GF_Read, windowX, windowY,  windowWidth, windowHeight,
              static_cast<void*>(reinterpret_cast<GByte*>(&rawUniformGridData[0]) +
              0 * bandSpace), destWidth, destHeight,
              this->TargetDataType, pixelSpace, lineSpace);
      assert(err == CE_None);
      err = greenBand->RasterIO(
              GF_Read, windowX, windowY,  windowWidth, windowHeight,
              static_cast<void*>(reinterpret_cast<GByte*>(&rawUniformGridData[0]) +
              1 * bandSpace), destWidth, destHeight,
              this->TargetDataType, pixelSpace, lineSpace);
      assert(err == CE_None);
      err = blueBand->RasterIO(
              GF_Read, windowX, windowY,  windowWidth, windowHeight,
              static_cast<void*>(reinterpret_cast<GByte*>(&rawUniformGridData[0]) +
              2 * bandSpace), destWidth, destHeight,
              this->TargetDataType, pixelSpace, lineSpace);
      assert(err == CE_None);
      err = alphaBand->RasterIO(
              GF_Read, windowX, windowY,  windowWidth, windowHeight,
              static_cast<void*>(reinterpret_cast<GByte*>(&rawUniformGridData[0]) +
              3 * bandSpace), destWidth, destHeight,
              this->TargetDataType, pixelSpace, lineSpace);
      assert(err == CE_None);
    }
    else
    {
      this->Reader->SetNumberOfScalarComponents(3);
      rawUniformGridData.resize(3 * destWidth * destHeight * pixelSpace);

      err = redBand->RasterIO(
              GF_Read, windowX, windowY,  windowWidth, windowHeight,
              static_cast<void*>(reinterpret_cast<GByte*>(&rawUniformGridData[0]) +
              0 * bandSpace), destWidth, destHeight,
              this->TargetDataType, 0, 0);
      assert(err == CE_None);
      err = greenBand->RasterIO(GF_Read, windowX, windowY,  windowWidth, windowHeight,
              static_cast<void*>(reinterpret_cast<GByte*>(&rawUniformGridData[0]) +
              1 * bandSpace), destWidth, destHeight,
              this->TargetDataType, 0,0);
      assert(err == CE_None);
      err = blueBand->RasterIO(GF_Read, windowX, windowY,  windowWidth, windowHeight,
              static_cast<void*>(reinterpret_cast<GByte*>(&rawUniformGridData[0]) +
              2 * bandSpace), destWidth, destHeight,
              this->TargetDataType, 0,0);
      assert(err == CE_None);
    }
  }
  else if (grayBand)
  {
    otherBands[grayIndex - 1] = nullptr;
    otherIndex[grayIndex - 1] = 0;
    groupIndex.push_back(grayIndex);
    if (alphaBand)
    {
      otherBands[alphaIndex - 1] = nullptr;
      otherIndex[alphaIndex - 1] = 0;
      groupIndex.push_back(alphaIndex);
      // Luminance alpha
      this->Reader->SetNumberOfScalarComponents(2);
      rawUniformGridData.resize(2 * destWidth * destHeight * pixelSpace);

      err = grayBand->RasterIO(
              GF_Read, windowX, windowY,  windowWidth, windowHeight,
              static_cast<void*>(reinterpret_cast<GByte*>(&rawUniformGridData[0]) +
              0 * bandSpace), destWidth, destHeight,
              this->TargetDataType, pixelSpace, lineSpace);
      assert(err == CE_None);
      err =  alphaBand->RasterIO(
               GF_Read, windowX, windowY,  windowWidth, windowHeight,
               static_cast<void*>(reinterpret_cast<GByte*>(&rawUniformGridData[0]) +
               1 * bandSpace), destWidth, destHeight,
               this->TargetDataType, pixelSpace, lineSpace);
      assert(err == CE_None);
    }
    else
    {
      // Luminance
      this->Reader->SetNumberOfScalarComponents(1);
      rawUniformGridData.resize(destWidth * destHeight * pixelSpace);
      err = grayBand->RasterIO(
              GF_Read, windowX, windowY,  windowWidth, windowHeight,
              static_cast<void*>(reinterpret_cast<GByte*>(&rawUniformGridData[0]) +
              0 * bandSpace), destWidth, destHeight,
              this->TargetDataType, pixelSpace, lineSpace);
      assert(err == CE_None);
    }
  }
  else if (paletteBand)
  {
    otherBands[paletteIndex - 1] = nullptr;
    otherIndex[paletteIndex - 1] = 0;
    groupIndex.push_back(paletteIndex);
    // Read indexes
    this->Reader->SetNumberOfScalarComponents(1);
    rawUniformGridData.resize(destWidth * destHeight * pixelSpace);
    err = paletteBand->RasterIO(
      GF_Read, windowX, windowY,  windowWidth, windowHeight,
      static_cast<void*>(reinterpret_cast<GByte*>(&rawUniformGridData[0]) +
      0 * bandSpace), destWidth, destHeight,
      this->TargetDataType, pixelSpace, lineSpace);
    assert(err == CE_None);

    this->ReadColorTable(paletteBand, colorTable);
  }

  (void)err; //unused

  const double* d = GetGeoCornerPoints();
  // 4,5 are the x,y coordinates for the opposite corner to 0,1
  double geoSpacing[] = {(d[4]-d[0])/this->Reader->RasterDimensions[0],
                         (d[5]-d[1])/this->Reader->RasterDimensions[1],
                         1};

  // destWidth, destHeight are the number of cells. Points are one more than cells
  this->UniformGridData->SetExtent(0, destWidth, 0, destHeight, 0, 0);
  this->UniformGridData->SetSpacing(std::abs(geoSpacing[0]), std::abs(geoSpacing[1]), geoSpacing[2]);
  this->UniformGridData->SetOrigin(std::min(d[0], d[4]), std::min(d[1], d[5]), 0);
  this->Convert<VTK_TYPE, RAW_TYPE>(rawUniformGridData, destWidth, destHeight,
                                    groupIndex, "Elevation", geoSpacing[0] < 0, geoSpacing[1] < 0);
  this->UniformGridData->GetCellData()->SetActiveScalars("Elevation");
  groupIndex.resize(1, 0);
  rawUniformGridData.resize(destWidth * destHeight * pixelSpace);
  for (size_t i = 0; i < otherBands.size(); ++i)
  {
    if (otherBands[i] != nullptr)
    {
      groupIndex[0] = otherIndex[i];
      err = otherBands[i]->RasterIO(
        GF_Read, windowX, windowY,  windowWidth, windowHeight,
        static_cast<void*>(reinterpret_cast<GByte*>(&rawUniformGridData[0]) +
                           0 * bandSpace), destWidth, destHeight,
                           this->TargetDataType, pixelSpace, lineSpace);
      assert(err == CE_None);
      this->Convert<VTK_TYPE, RAW_TYPE>(
        rawUniformGridData, destWidth, destHeight,
        groupIndex, (std::string("band_") + std::to_string(otherIndex[i])).c_str(),
        geoSpacing[0] < 0, geoSpacing[1] < 0);
    }
  }

  if (paletteBand)
  {
    this->UniformGridData->GetCellData()->GetScalars()->SetName("Categories");
    this->UniformGridData->GetCellData()->GetScalars()->SetLookupTable(
      colorTable);
  }
}

//----------------------------------------------------------------------------
void vtkGDALRasterReader::vtkGDALRasterReaderInternal::ReleaseData()
{
  GDALClose(this->GDALData);
}

//-----------------------------------------------------------------------------
template <typename VTK_TYPE, typename RAW_TYPE>
void vtkGDALRasterReader::vtkGDALRasterReaderInternal::Convert(
  std::vector<RAW_TYPE>& rawUniformGridData, int targetWidth,
  int targetHeight, const std::vector<int>& groupIndex, const char* name, bool flipX, bool flipY)
{
  if (!this->UniformGridData)
  {
    return;
  }

  double targetIndex;
  double sourceIndex;
  double min = rawUniformGridData[0], max = rawUniformGridData[0];

  vtkSmartPointer<VTK_TYPE> scArr(vtkSmartPointer<VTK_TYPE>::New());
  scArr->SetName(name);
  scArr->SetNumberOfComponents(groupIndex.size());
  scArr->SetNumberOfTuples(targetWidth * targetHeight);

  for (int j = 0; j < targetHeight; ++j)
  {
    int jIndex = flipY ? (targetHeight - 1 - j) : j;
    for (int i = 0; i < targetWidth; ++i)
    {
      int iIndex = flipX ? (targetWidth - 1 - i) : i;
      // Each band GDALData is stored in width * height size array.
      for (int bi = 0; bi < groupIndex.size(); ++bi)
      {
        int bandIndex = groupIndex[bi];
        RAW_TYPE TNoDataValue = 0;
        if (this->HasNoDataValue[bandIndex - 1])
        {
          TNoDataValue = static_cast<RAW_TYPE>(this->NoDataValue[bandIndex - 1]);
        }

        targetIndex = i * groupIndex.size() +
          j * targetWidth * groupIndex.size() + bi;
        sourceIndex = iIndex + jIndex * targetWidth +
                      bi * targetWidth * targetHeight;

        RAW_TYPE tmp = rawUniformGridData[sourceIndex];
        if (this->HasNoDataValue[bandIndex - 1] && tmp == TNoDataValue)
        {
          this->UniformGridData->BlankCell(targetIndex);
        }
        else
        {
          if(tmp < min) min = tmp;
          if(tmp > max) max = tmp;
          this->NumberOfCells++;
        }

        scArr->InsertValue(targetIndex, rawUniformGridData[sourceIndex]);
      }
    }
  }
  this->UniformGridData->GetCellData()->AddArray(scArr);
}

//-----------------------------------------------------------------------------
bool vtkGDALRasterReader::vtkGDALRasterReaderInternal::GetGeoCornerPoint(
  GDALDataset* dataset, double x, double y, double* out) const
{
  bool retVal = false;

  if (!dataset)
  {
    std::cerr << "Empty GDAL dataset" << std::endl;
    return retVal;
  }

  if (!out)
  {
    return retVal;
  }

  double dfGeoX = 0;
  double dfGeoY = 0;
  double adfGeoTransform[6];

  const char *gcpProj = this->GDALData->GetGCPProjection();
  const GDAL_GCP *gcps = this->GDALData->GetGCPs();

  if (gcpProj == nullptr || gcps == nullptr)
  {
    // Transform the point into georeferenced coordinates
    if (GDALGetGeoTransform(this->GDALData, adfGeoTransform) == CE_None)
    {
      dfGeoX = adfGeoTransform[0] + adfGeoTransform[1] * x +
        adfGeoTransform[2] * y;
      dfGeoY = adfGeoTransform[3] + adfGeoTransform[4] * x +
        adfGeoTransform[5] * y;

      retVal = true;
    }
    else
    {
      dfGeoX = x;
      dfGeoY = y;

      retVal = false;
    }
  }
  else
  {
    // 1st pass: we should really have a call to the reader that returns
    // the homography, but for now, look for mathcing corner and pass back
    // the matching corner point ("0" pixel on input means "0.5" as far as
    // GDAL goes
    bool leftCorner = (x == 0);
    bool upperCorner = (y == 0);
    for (int i = 0; i < 4; ++i)
    {
      bool gcpLeftCorner = (gcps[i].dfGCPPixel == 0.5);
      bool gcpUpperCorner = (gcps[i].dfGCPLine == 0.5);
      if (gcpLeftCorner == leftCorner && gcpUpperCorner == upperCorner)
      {
        dfGeoX = gcps[i].dfGCPX;
        dfGeoY = gcps[i].dfGCPY;
      }
    }
  }

  out[0] = dfGeoX;
  out[1] = dfGeoY;

  return retVal;
}

//-----------------------------------------------------------------------------
const double* vtkGDALRasterReader::vtkGDALRasterReaderInternal::GetGeoCornerPoints()
{
  this->GetGeoCornerPoint(this->GDALData, 0,
                          0,
                          &this->CornerPoints[0]);
  this->GetGeoCornerPoint(this->GDALData, 0,
                          this->Reader->RasterDimensions[1],
                          &this->CornerPoints[2]);
  this->GetGeoCornerPoint(this->GDALData,
                          this->Reader->RasterDimensions[0],
                          this->Reader->RasterDimensions[1],
                          &this->CornerPoints[4]);
  this->GetGeoCornerPoint(this->GDALData,
                          this->Reader->RasterDimensions[0],
                          0,
                          &this->CornerPoints[6]);

  return this->CornerPoints;
}

//-----------------------------------------------------------------------------
void vtkGDALRasterReader::vtkGDALRasterReaderInternal::ReadColorTable(
  GDALRasterBand *rasterBand, vtkLookupTable *colorTable) const
{
  GDALColorTable *gdalTable = rasterBand->GetColorTable();
  if (gdalTable->GetPaletteInterpretation() != GPI_RGB)
  {
    std::cerr << "Color table palette type not supported "
              << gdalTable->GetPaletteInterpretation() << std::endl;
    return;
  }

  char **categoryNames = rasterBand->GetCategoryNames();

  colorTable->IndexedLookupOn();
  int numEntries = gdalTable->GetColorEntryCount();
  colorTable->SetNumberOfTableValues(numEntries);
  std::stringstream ss;
  for (int i=0; i< numEntries; ++i)
  {
    const GDALColorEntry *gdalEntry = gdalTable->GetColorEntry(i);
    double r = static_cast<double>(gdalEntry->c1) / 255.0;
    double g = static_cast<double>(gdalEntry->c2) / 255.0;
    double b = static_cast<double>(gdalEntry->c3) / 255.0;
    double a = static_cast<double>(gdalEntry->c4) / 255.0;
    colorTable->SetTableValue(i, r, g, b, a);

    // Copy category name to lookup table annotation
    if (categoryNames)
    {
      // Only use non-empty names
      if (strlen(categoryNames[i]) > 0)
      {
        colorTable->SetAnnotation(vtkVariant(i), categoryNames[i]);
      }
    }
    else
    {
      // Create default annotation
      ss.str("");
      ss.clear();
      ss << "Category " << i;
      colorTable->SetAnnotation(vtkVariant(i), ss.str());
    }
  }
}

//-----------------------------------------------------------------------------
void vtkGDALRasterReader::PrintSelf(std::ostream& os, vtkIndent indent)
{
  this->Superclass::PrintSelf(os,indent);

  os << indent << "TargetDimensions: " <<
    this->TargetDimensions[0] << indent << this->TargetDimensions[1] << "\n";
  os << indent << "TargetDimensions: " <<
    this->RasterDimensions[0] << indent << this->RasterDimensions[1] << "\n";
  os << indent << "DomainMetaData: " << this->DomainMetaData << "\n";
  os << indent << "DriverShortName: " << this->DriverShortName << "\n";
  os << indent << "DriverLongName: " << this->DriverLongName << "\n";

  if (!this->Domains.empty())
  {
    os << indent << "Domain" << "\n";
    for (std::size_t i = 0; i < this->Domains.size(); ++i)
    {
      os << indent << this->Domains[i] << "\n";
    }
  }

  if (!this->MetaData.empty())
  {
    os << indent << "MetaData" << "\n";
    for (std::size_t i = 0; i < this->MetaData.size(); ++i)
    {
      os << indent << this->MetaData[i] << "\n";
    }
  }
}

//-----------------------------------------------------------------------------
vtkGDALRasterReader::vtkGDALRasterReader() : vtkImageReader2(),
  Implementation(0)
{
  this->Implementation = new vtkGDALRasterReaderInternal(this);

  this->SetNumberOfInputPorts(0);
  this->SetNumberOfOutputPorts(1);

  this->DataOrigin[0] = 0.0;
  this->DataOrigin[1] = 0.0;
  this->DataOrigin[2] = 0.0;

  this->DataSpacing[0] = 1.0;
  this->DataSpacing[1] = 1.0;
  this->DataSpacing[2] = 1.0;

  this->DataExtent[0] = -1;
  this->DataExtent[1] = -1;
  this->DataExtent[2] = -1;
  this->DataExtent[3] = -1;
  this->DataExtent[4] = -1;
  this->DataExtent[5] = -1;

  this->TargetDimensions[0] = -1;
  this->TargetDimensions[1] = -1;

  this->RasterDimensions[0] = -1;
  this->RasterDimensions[1] = -1;
}

//-----------------------------------------------------------------------------
vtkGDALRasterReader::~vtkGDALRasterReader()
{
  delete this->Implementation;

  if (this->FileName)
  {
    this->SetFileName(0);
  }
}


//-----------------------------------------------------------------------------
int vtkGDALRasterReader::CanReadFile(const char* fname)
{
  GDALDataset* dataset = static_cast<GDALDataset*>(GDALOpen(fname, GA_ReadOnly));
  bool canRead = (dataset != nullptr);
  GDALClose(dataset);
  return canRead;
}

//-----------------------------------------------------------------------------
const char* vtkGDALRasterReader::GetProjectionString() const
{
  return this->Projection.c_str();
}

//-----------------------------------------------------------------------------
const double* vtkGDALRasterReader::GetGeoCornerPoints()
{
  return this->Implementation->GetGeoCornerPoints();
}

//-----------------------------------------------------------------------------
const std::vector<std::string>& vtkGDALRasterReader::GetMetaData()
{
  return this->MetaData;
}

//-----------------------------------------------------------------------------
std::vector<std::string> vtkGDALRasterReader::GetDomainMetaData(
  const std::string& domain)
{
  std::vector<std::string> domainMetaData;

  char** papszMetadata =
    GDALGetMetadata(this->Implementation->GDALData, domain.c_str());

  if (CSLCount(papszMetadata) > 0)
  {
    for (int i = 0; papszMetadata[i] != nullptr; ++i)
    {
      domainMetaData.push_back(papszMetadata[i]);
    }
  }

  return domainMetaData;
}

//-----------------------------------------------------------------------------
const std::string& vtkGDALRasterReader::GetDriverShortName()
{
  return this->DriverShortName;
}

//-----------------------------------------------------------------------------
const std::string& vtkGDALRasterReader::GetDriverLongName()
{
  return this->DriverLongName;
}

vtkIdType vtkGDALRasterReader::GetNumberOfCells()
{
  return this->Implementation->NumberOfCells;
}

#ifdef _MSC_VER
#define strdup _strdup
#endif

//-----------------------------------------------------------------------------
int vtkGDALRasterReader::RequestData(vtkInformation* vtkNotUsed(request),
                               vtkInformationVector** vtkNotUsed(inputVector),
                               vtkInformationVector* outputVector)
{
  if (this->TargetDimensions[0] <= 0 || this->TargetDimensions[1] <= 0)
  {
    vtkWarningMacro( << "Invalid target dimensions")
  }

  this->Implementation->ReadData(this->FileName);
  if (!this->Implementation->GDALData)
  {
    vtkErrorMacro("Failed to read "  << this->FileName);
    return 0;
  }

  // Get the projection.
  char* proj = strdup(this->Implementation->GDALData->GetProjectionRef());
  OGRSpatialReference spRef(proj);
  free(proj);

  char* projection;
  spRef.exportToProj4(&projection);
  this->Projection = projection;
  CPLFree(projection);

  // Add the map-projection as field data
  vtkSmartPointer<vtkStringArray> projectionData =
    vtkSmartPointer<vtkStringArray>::New();
  projectionData->SetName("MAP_PROJECTION");
  projectionData->SetNumberOfComponents(1);
  projectionData->SetNumberOfTuples(1);
  projectionData->SetValue(0, this->Projection);
  this->Implementation->UniformGridData->GetFieldData()->AddArray(
    projectionData);

  // Add NoDataValue as field data
  // GDALDatset can have 1 value for each raster band
  // Use NaN for undefined values
  vtkSmartPointer<vtkDoubleArray> noDataArray =
    vtkSmartPointer<vtkDoubleArray>::New();
  noDataArray->SetName("NO_DATA_VALUE");
  noDataArray->SetNumberOfComponents(1);
  noDataArray->SetNumberOfTuples(this->Implementation->NumberOfBands);
  for (int i=0; i<this->Implementation->NumberOfBands; ++i)
  {
    int success = 0;
    double noDataValue = this->Implementation->GDALData->GetRasterBand(
      i+1)->GetNoDataValue(&success);
    if (!success)
    {
      noDataValue = vtkMath::Nan();
    }
     noDataArray->SetValue(i, noDataValue);
  }
  this->Implementation->UniformGridData->GetFieldData()->AddArray(
    noDataArray);

  // Check if file has been changed here.
  // If changed then throw the vtxId time and load a new one.
  vtkInformation* outInfo = outputVector->GetInformationObject(0);
  if (!outInfo)
  {
    return 0;
  }

  vtkDataObject* dataObj = outInfo->Get(vtkDataObject::DATA_OBJECT());
  if (!dataObj)
  {
    return 0;
  }

  vtkUniformGrid::SafeDownCast(dataObj)->ShallowCopy(this->Implementation->UniformGridData);
  return 1;
}

//-----------------------------------------------------------------------------
int vtkGDALRasterReader::RequestInformation(vtkInformation * vtkNotUsed(request),
  vtkInformationVector **vtkNotUsed(inputVector),
  vtkInformationVector *outputVector)
{
  // get the info objects
  vtkInformation* outInfo = outputVector->GetInformationObject(0);
  if (!outInfo)
  {
    vtkErrorMacro("Invalid output information object");
    return 0;
  }

  if (!this->FileName)
  {
    vtkErrorMacro("Requires valid input file name") ;
    return 0;
  }

  this->Implementation->ReadMetaData(this->FileName);
  if (!this->Implementation->GDALData)
  {
    vtkErrorMacro("Failed to read "  << this->FileName);
    return 0;
  }

  if (this->RasterDimensions[0] <= 0 &&
      this->RasterDimensions[1] <= 0)
  {
    vtkErrorMacro("Invalid image dimensions");
    return 0;
  }

  if (this->TargetDimensions[0] == -1 &&
      this->TargetDimensions[1] == -1)
  {
    this->TargetDimensions[0] = this->RasterDimensions[0];
    this->TargetDimensions[1] = this->RasterDimensions[1];
  }

  if (this->DataExtent[0] == -1)
  {
    this->DataExtent[0] = 0;
    this->DataExtent[1] = this->RasterDimensions[0] - 1;
    this->DataExtent[2] = 0;
    this->DataExtent[3] = this->RasterDimensions[1] - 1;
    this->DataExtent[4] = 0;
    this->DataExtent[5] = 0;
  }

  // GDAL top left is at 0,0
  this->Implementation->SourceOffset[0] = this->DataExtent[0];
  this->Implementation->SourceOffset[1] = this->RasterDimensions[1] -
                                          (this->DataExtent[3] + 1);

  this->Implementation->SourceDimensions[0] =
    this->DataExtent[1] - this->DataExtent[0] + 1;
  this->Implementation->SourceDimensions[1] =
    this->DataExtent[3] - this->DataExtent[2] + 1;

  // Clamp pixel offset and image dimension
  this->Implementation->SourceOffset[0] =
    Min(this->RasterDimensions[0], this->Implementation->SourceOffset[0]);
  this->Implementation->SourceOffset[0] =
    Max(0.0, this->Implementation->SourceOffset[0]);

  this->Implementation->SourceOffset[1] =
    Min(this->RasterDimensions[1], this->Implementation->SourceOffset[1]);
  this->Implementation->SourceOffset[1] =
    Max(0.0, this->Implementation->SourceOffset[1]);

  this->Implementation->SourceDimensions[0] =
    Max(0.0, this->Implementation->SourceDimensions[0]);
  if ((this->Implementation->SourceDimensions[0] +
       this->Implementation->SourceOffset[0]) >
      this->RasterDimensions[0])
  {
    this->Implementation->SourceDimensions[0] =
      this->RasterDimensions[0] - this->Implementation->SourceOffset[0];
  }

  this->Implementation->SourceDimensions[1] =
    Max(0.0, this->Implementation->SourceDimensions[1]);
  if ((this->Implementation->SourceDimensions[1] +
       this->Implementation->SourceOffset[1]) >
      this->RasterDimensions[1])
  {
    this->Implementation->SourceDimensions[1] =
      this->RasterDimensions[1] - this->Implementation->SourceOffset[1];
  }

  this->DataExtent[0] = this->Implementation->SourceOffset[0];
  this->DataExtent[1] = this->DataExtent[0] +
                         this->Implementation->SourceDimensions[0] - 1;
  this->DataExtent[3] = this->RasterDimensions[1] -
                         this->Implementation->SourceOffset[1] - 1;
  this->DataExtent[2] = this->DataExtent[3] -
                         this->Implementation->SourceDimensions[1] + 1;
  this->DataExtent[4] = 0;
  this->DataExtent[5] = 0;

  double geoTransform[6] = {};
  if (CE_Failure == this->Implementation->GDALData->GetGeoTransform(geoTransform))
  {
    // Issue warning message if image doensn't contain geotransform.
    // Not fatal because GDAL will return a default transform on CE_Failure.
    vtkErrorMacro("No GeoTransform data in input image");
  }
  this->DataOrigin[0] = geoTransform[0];
  this->DataOrigin[1] = geoTransform[3];
  this->DataOrigin[2] = 0.0;
  this->DataSpacing[0] = geoTransform[1];
  this->DataSpacing[1] = geoTransform[5];
  this->DataSpacing[2] = 0.0;

  outInfo->Set(vtkStreamingDemandDrivenPipeline::WHOLE_EXTENT(),
               this->DataExtent, 6);
  outInfo->Set(vtkDataObject::SPACING(), this->DataSpacing, 3);
  outInfo->Set(vtkDataObject::ORIGIN(), this->DataOrigin, 3);
  outInfo->Set(vtkGDAL::MAP_PROJECTION(),
               this->Implementation->GDALData->GetProjectionRef());

  return 1;
}

//-----------------------------------------------------------------------------
int vtkGDALRasterReader::FillOutputPortInformation(int port, vtkInformation* info)
{
  if (port == 0)
  {
    info->Set(vtkDataObject::DATA_TYPE_NAME(), "vtkUniformGrid");
    //info->Set(vtkDataObject::DATA_TYPE_NAME(), "vtkImageData");
    return 1;
  }
  else
  {
    vtkErrorMacro("Port: " << port << " is not a valid port");
    return 0;
  }
}

//-----------------------------------------------------------------------------
double vtkGDALRasterReader::GetInvalidValue(int bandIndex)
{
  return Implementation->NoDataValue[bandIndex - 1];
}
