/*=========================================================================

  Program:   Visualization Toolkit
  Module:    vtkImageCachedSource.h
  Language:  C++
  Date:      $Date$
  Version:   $Revision$
  Thanks:    Thanks to C. Charles Law who developed this class.

Copyright (c) 1993-1995 Ken Martin, Will Schroeder, Bill Lorensen.

This software is copyrighted by Ken Martin, Will Schroeder and Bill Lorensen.
The following terms apply to all files associated with the software unless
explicitly disclaimed in individual files. This copyright specifically does
not apply to the related textbook "The Visualization Toolkit" ISBN
013199837-4 published by Prentice Hall which is covered by its own copyright.

The authors hereby grant permission to use, copy, and distribute this
software and its documentation for any purpose, provided that existing
copyright notices are retained in all copies and that this notice is included
verbatim in any distributions. Additionally, the authors grant permission to
modify this software and its documentation for any purpose, provided that
such modifications are not distributed without the explicit consent of the
authors and that existing copyright notices are retained in all copies. Some
of the algorithms implemented by this software are patented, observe all
applicable patent law.

IN NO EVENT SHALL THE AUTHORS OR DISTRIBUTORS BE LIABLE TO ANY PARTY FOR
DIRECT, INDIRECT, SPECIAL, INCIDENTAL, OR CONSEQUENTIAL DAMAGES ARISING OUT
OF THE USE OF THIS SOFTWARE, ITS DOCUMENTATION, OR ANY DERIVATIVES THEREOF,
EVEN IF THE AUTHORS HAVE BEEN ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

THE AUTHORS AND DISTRIBUTORS SPECIFICALLY DISCLAIM ANY WARRANTIES, INCLUDING,
BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY, FITNESS FOR A
PARTICULAR PURPOSE, AND NON-INFRINGEMENT.  THIS SOFTWARE IS PROVIDED ON AN
"AS IS" BASIS, AND THE AUTHORS AND DISTRIBUTORS HAVE NO OBLIGATION TO PROVIDE
MAINTENANCE, SUPPORT, UPDATES, ENHANCEMENTS, OR MODIFICATIONS.


=========================================================================*/
// .NAME vtkImageCachedSourceB - Source of data for pipeline.
// .SECTION Description
// vtkImageCachedSourceB is a source that has two output caches.  It is
// an experiment to see if this is a viable alternative to a 5th dimension.


#ifndef __vtkImageCachedSource_h
#define __vtkImageCachedSource_h

#include "vtkObject.h"
#include "vtkImageSource.h"
#include "vtkImageRegion.h"
class vtkImageCache;


class vtkImageCachedSource : public vtkObject
{
public:
  vtkImageCachedSource();
  ~vtkImageCachedSource();
  char *GetClassName() {return "vtkImageCachedSource";};
  void PrintSelf(ostream& os, vtkIndent indent);

  virtual void InterceptCacheUpdate(vtkImageRegion *region);
  virtual void UpdatePointData(int dim, vtkImageRegion *region); 
  virtual void UpdateImageInformation(vtkImageRegion *region) = 0;

  virtual unsigned long GetPipelineMTime();
  vtkImageSource *GetOutput();

  virtual void SetCache(vtkImageCache *cache);
  vtkImageCache *GetCache();

  virtual void SetReleaseDataFlag(int value);
  int  GetReleaseDataFlag();
  vtkBooleanMacro(ReleaseDataFlag, int);
  
  void SetOutputScalarType(int type);
  int  GetOutputScalarType();
  
  // Set/Get the coordinate system for this filter.
  void SetAxes(int dim, int *axes);
  vtkImageSetMacro(Axes,int);
  void GetAxes(int dim, int *axes);
  vtkImageGetMacro(Axes,int);

  void SetOutputMemoryLimit(long limit);

protected:
  vtkImageCache *Output;
  int NumberOfAxes;               // execute method expects this number of axes
  int Axes[VTK_IMAGE_DIMENSIONS]; // reorder the axes
  
  virtual void UpdatePointData(vtkImageRegion *region); 
  virtual void CheckCache();
};


// all necessary header files will be included automatically.
#include "vtkImageCache.h"

#endif


