/*=========================================================================

  Program:   Visualization Toolkit
  Module:    vtkRenderer.cxx
  Language:  C++
  Date:      $Date$
  Version:   $Revision$


Copyright (c) 1993-1998 Ken Martin, Will Schroeder, Bill Lorensen.

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
#include <stdlib.h>
#include <string.h>

#include "vtkRenderer.h"
#include "vtkRenderWindow.h"
#include "vtkMath.h"
#include "vtkVolume.h"
#include "vtkRayCaster.h"
#include "vtkTimerLog.h"
#include "vtkCuller.h"

// Create a vtkRenderer with a black background, a white ambient light, 
// two-sided lighting turned on, a viewport of (0,0,1,1), and backface culling
// turned off.
vtkRenderer::vtkRenderer()
{
  this->ActiveCamera = NULL;

  this->Ambient[0] = 1;
  this->Ambient[1] = 1;
  this->Ambient[2] = 1;

  this->RayCaster = vtkRayCaster::New();
  this->RayCaster->SetRenderer( this );

  this->AllocatedRenderTime = 100;
  
  this->CreatedLight = NULL;
  
  this->TwoSidedLighting = 1;
  this->BackingStore = 0;
  this->BackingImage = NULL;
  this->LastRenderTimeInSeconds = -1.0;
  
  this->RenderWindow = NULL;
  this->Lights = vtkLightCollection::New();
  this->Actors = vtkActorCollection::New();
  this->Volumes = vtkVolumeCollection::New();

  this->Cullers = vtkCullerCollection::New();  
}

vtkRenderer::~vtkRenderer()
{
  this->SetRenderWindow( NULL );
  
  if (this->ActiveCamera)
    {
    this->ActiveCamera->UnRegister(this);
    this->ActiveCamera = NULL;
    }

  if (this->CreatedLight)
    {
    this->CreatedLight->UnRegister(this);
    this->CreatedLight = NULL;
    }

  // unregister actually should take care of rayCaster
  if (this->RayCaster)
    {
    this->RayCaster->Delete();
    }
  if (this->BackingImage)
    {
    delete [] this->BackingImage;
    }
  
  this->Actors->Delete();
  this->Actors = NULL;
  this->Volumes->Delete();
  this->Volumes = NULL;
  this->Lights->Delete();
  this->Lights = NULL;
  this->Cullers->Delete();
  this->Cullers = NULL;
}

#ifdef VTK_USE_OGLR
#include "vtkOpenGLRenderer.h"
#endif
#ifdef _WIN32
#include "vtkOpenGLRenderer.h"
#endif
// return the correct type of Renderer 
vtkRenderer *vtkRenderer::New()
{
  char *temp = vtkRenderWindow::GetRenderLibrary();
  
#ifdef VTK_USE_OGLR
  if (!strcmp("OpenGL",temp))
    {
    return vtkOpenGLRenderer::New();
    }
#endif
#ifdef _WIN32
  if (!strcmp("Win32OpenGL",temp))
    {
    return vtkOpenGLRenderer::New();
    }
#endif
  
  return new vtkRenderer;
}

// Concrete render method.
void vtkRenderer::Render(void)
{
  double   t1, t2;
  int      i;
  vtkProp  *aProp;

  t1 = vtkTimerLog::GetCurrentTime();

  if (this->StartRenderMethod) 
    {
    (*this->StartRenderMethod)(this->StartRenderMethodArg);
    }

  // if backing store is on and we have a stored image
  if (this->BackingStore && this->BackingImage &&
      this->MTime < this->RenderTime &&
      this->ActiveCamera->GetMTime() < this->RenderTime &&
      this->RenderWindow->GetMTime() < this->RenderTime)
    {
    int mods = 0;
    vtkLight *light;
    vtkProp *aProp;
    
    // now we just need to check the lights and actors
    for(this->Lights->InitTraversal(); 
	(light = this->Lights->GetNextItem()); )
      {
      if (light->GetSwitch() && 
	  light->GetMTime() > this->RenderTime)
	{
	mods = 1;
	}
      }
    for (this->Props->InitTraversal(); 
	 (aProp = this->Props->GetNextProp()); )
      {
      // if it's invisible, we can skip the rest 
      if (aProp->GetVisibility())
	{
	if (aProp->GetRedrawMTime() > this->RenderTime)
	  {
	  mods = 1;
	  }
	}
      }
    
    if (!mods)
      {
      int rx1, ry1, rx2, ry2;
      
      // backing store should be OK, lets use it
      // calc the pixel range for the renderer
      rx1 = (int)(this->Viewport[0]*(this->RenderWindow->GetSize()[0] - 1));
      ry1 = (int)(this->Viewport[1]*(this->RenderWindow->GetSize()[1] - 1));
      rx2 = (int)(this->Viewport[2]*(this->RenderWindow->GetSize()[0] - 1));
      ry2 = (int)(this->Viewport[3]*(this->RenderWindow->GetSize()[1] - 1));
      this->RenderWindow->SetPixelData(rx1,ry1,rx2,ry2,this->BackingImage,0);
      if (this->EndRenderMethod) 
       {
       (*this->EndRenderMethod)(this->EndRenderMethodArg);
       }
      return;
      }
    }

  // Create the initial list of visible props
  // This will be passed through AllocateTime(), where
  // a time is allocated for each prop, and the list
  // maybe re-ordered by the cullers. Also create the
  // sublists for the props that need ray casting, and
  // the props that need to be rendered into an image.
  // Fill these in later (in AllocateTime) - get a 
  // count of them there too
  this->PropArray                = new vtkProp *[this->Props->GetNumberOfItems()];
  this->RayCastPropArray         = new vtkProp *[this->Props->GetNumberOfItems()];
  this->RenderIntoImagePropArray = new vtkProp *[this->Props->GetNumberOfItems()];
  this->PropArrayCount = 0;

  for ( i = 0, this->Props->InitTraversal(); 
	(aProp = this->Props->GetNextProp());i++ )
    {
    if ( aProp->GetVisibility() )
      {
      this->PropArray[this->PropArrayCount++] = aProp;
      }
    }
  
  if ( this->PropArrayCount == 0 )
    {
    vtkDebugMacro( << "There are no visible props!" );
    }

  // Call all the outer culling methods to set allocated time
  // for each prop and re-order the prop list if desired
  this->AllocateTime();

  // do the render library specific stuff
  this->DeviceRender();

  // Clean up the space we allocated before
  delete [] this->PropArray;
  delete [] this->RayCastPropArray;
  delete [] this->RenderIntoImagePropArray;

  if (this->BackingStore)
    {
    if (this->BackingImage)
      {
      delete [] this->BackingImage;
      }
    
    int rx1, ry1, rx2, ry2;
    
    // backing store should be OK, lets use it
    // calc the pixel range for the renderer
    rx1 = (int)(this->Viewport[0]*(this->RenderWindow->GetSize()[0] - 1));
    ry1 = (int)(this->Viewport[1]*(this->RenderWindow->GetSize()[1] - 1));
    rx2 = (int)(this->Viewport[2]*(this->RenderWindow->GetSize()[0] - 1));
    ry2 = (int)(this->Viewport[3]*(this->RenderWindow->GetSize()[1] - 1));
    this->BackingImage = this->RenderWindow->GetPixelData(rx1,ry1,rx2,ry2,0);
    }
    

  t2 = vtkTimerLog::GetCurrentTime();

  this->LastRenderTimeInSeconds = (float) (t2 - t1);
}

void vtkRenderer::RenderOverlay()
{
  vtkProp *aProp;
  
  for (this->Props->InitTraversal(); 
       (aProp = this->Props->GetNextProp()); )
    {
    aProp->RenderOverlay(this);
    }

			 if (this->EndRenderMethod) 
    {
    (*this->EndRenderMethod)(this->EndRenderMethodArg);
    }
  this->RenderTime.Modified();
}

// Ask active camera to load its view matrix.
int vtkRenderer::UpdateCamera ()
{
  if (!this->ActiveCamera)
    {
    vtkDebugMacro(<< "No cameras are on, creating one.");
    // the get method will automagically create a camera
    // and reset it since one hasn't been specified yet
    this->GetActiveCamera();
    }

  // update the viewing transformation
  this->ActiveCamera->Render((vtkRenderer *)this);

  return 1;
}

// Do all outer culling to set allocated time for each prop.
// Possibly re-order the actor list.
void vtkRenderer::AllocateTime()
{
  int          initialized = 0;
  float        renderTime;
  float        totalTime;
  int          i;
  vtkCuller    *aCuller;
  vtkProp      *aProp;

  // Give each of the cullers a chance to modify allocated rendering time
  // for the entire set of props. Each culler returns the total time given
  // by AllocatedRenderTime for all props. Each culler is required to
  // place any props that have an allocated render time of 0.0 
  // at the end of the list. The PropArrayCount value that is
  // returned is the number of non-zero, visible actors.
  // Some cullers may do additional sorting of the list (by distance,
  // importance, etc).
  //
  // The first culler will initialize all the allocated render times. 
  // Any subsequent culling will multiply the new render time by the 
  // existing render time for an actor.

  totalTime = this->PropArrayCount;

  for (this->Cullers->InitTraversal(); 
       (aCuller=this->Cullers->GetNextItem());)
    {
    totalTime = 
      aCuller->Cull((vtkRenderer *)this, 
		    this->PropArray, this->PropArrayCount,
		    initialized );
    }

  // loop through all props and set the AllocatedRenderTime
  for ( i = 0; i < this->PropArrayCount; i++ )
    {
    aProp = this->PropArray[i];

    // If we don't have an outer cull method in any of the cullers,
    // then the allocated render time has not yet been initialized
    renderTime = (initialized)?(aProp->GetRenderTimeMultiplier()):(1.0);

    // We need to divide by total time so that the total rendering time
    // (all prop's AllocatedRenderTime added together) would be equal
    // to the renderer's AllocatedRenderTime.
    aProp->
      SetAllocatedRenderTime(( renderTime / totalTime ) * 
			     this->AllocatedRenderTime );  
    }

  // Since we now have allocated render times, we can select an LOD
  // (if this is an LODProp3D). We can now count up how many props need
  // ray casting or need to be rendered into an image and create
  // an array of them for fast traversal in the ray caster
  this->NumberOfPropsToRayCast = 0;
  this->NumberOfPropsToRenderIntoImage = 0;
  for ( i = 0; i < this->PropArrayCount; i++ )
    {    
    aProp = this->PropArray[i];
    if ( aProp->RequiresRayCasting() )
      {
      this->RayCastPropArray[this->NumberOfPropsToRayCast++] = aProp; 
      }
    
    if ( aProp->RequiresRenderingIntoImage() )
      {
      this->RenderIntoImagePropArray[this->NumberOfPropsToRenderIntoImage++] = aProp; 
      }
    }
}

// Ask actors to render themselves. As a side effect will cause 
// visualization network to update.
int vtkRenderer::UpdateGeometry()
{
  int        i;

  this->NumberOfPropsRenderedAsGeometry = 0;

  if ( this->PropArrayCount == 0 ) 
    {
    return 0;
    }

  // We can render everything because if it was
  // not visible it would not have been put in the
  // list in the first place, and if it was allocated
  // no time (culled) it would have been removed from
  // the list

  // loop through props and give them a change to 
  // render themselves as opaque geometry
  for ( i = 0; i < this->PropArrayCount; i++ )
    {
    this->NumberOfPropsRenderedAsGeometry += 
      this->PropArray[i]->RenderOpaqueGeometry(this);
    }


  // loop through props and give them a chance to 
  // render themselves as translucent geometry
  for ( i = 0; i < this->PropArrayCount; i++ )
    {
    this->NumberOfPropsRenderedAsGeometry += 
      this->PropArray[i]->RenderTranslucentGeometry(this);
    }

  vtkDebugMacro( << "Rendered " << 
                    this->NumberOfPropsRenderedAsGeometry << " actors" );

  return  this->NumberOfPropsRenderedAsGeometry;
}

vtkWindow *vtkRenderer::GetVTKWindow()
{
  return this->RenderWindow;
}

// Specify the camera to use for this renderer.
void vtkRenderer::SetActiveCamera(vtkCamera *cam)
{
  if (this->ActiveCamera == cam)
    {
    return;
    }

  if (this->ActiveCamera)
    {
    this->ActiveCamera->UnRegister(this);
    this->ActiveCamera = NULL;
    }
  if (cam)
    {
    cam->Register(this);
    }

  this->ActiveCamera = cam;
  this->Modified();
}

// Get the current camera.
vtkCamera *vtkRenderer::GetActiveCamera()
{
  if ( this->ActiveCamera == NULL )
    {
    this->ActiveCamera = vtkCamera::New();
    this->ResetCamera();
    }

  return this->ActiveCamera;
}

// Add a light to the list of lights.
void vtkRenderer::AddLight(vtkLight *light)
{
  this->Lights->AddItem(light);
}

// look through the props and get all the actors
vtkActorCollection *vtkRenderer::GetActors()
{
  vtkProp *aProp;
  
  // clear the collection first
  this->Actors->RemoveAllItems();
  
  for (this->Props->InitTraversal(); 
       (aProp = this->Props->GetNextProp()); )
    {
    aProp->GetActors(this->Actors);
    }
  return this->Actors;
}

// look through the props and get all the actors
vtkVolumeCollection *vtkRenderer::GetVolumes()
{
  vtkProp *aProp;
  
  // clear the collection first
  this->Volumes->RemoveAllItems();
  
  for (this->Props->InitTraversal(); 
       (aProp = this->Props->GetNextProp()); )
    {
    aProp->GetVolumes(this->Volumes);
    }
  return this->Volumes;
}

// Remove a light from the list of lights.
void vtkRenderer::RemoveLight(vtkLight *light)
{
  this->Lights->RemoveItem(light);
}

// Add an culler to the list of cullers.
void vtkRenderer::AddCuller(vtkCuller *culler)
{
  this->Cullers->AddItem(culler);
}

// Remove an actor from the list of cullers.
void vtkRenderer::RemoveCuller(vtkCuller *culler)
{
  this->Cullers->RemoveItem(culler);
}

void vtkRenderer::CreateLight(void)
{
  if (this->CreatedLight)
    {
    this->CreatedLight->UnRegister(this);
    this->CreatedLight = NULL;
    }

  this->CreatedLight = vtkLight::New();
  this->AddLight(this->CreatedLight);
  this->CreatedLight->SetPosition(this->ActiveCamera->GetPosition());
  this->CreatedLight->SetFocalPoint(this->ActiveCamera->GetFocalPoint());
}

// Compute the bounds of the visibile props
void vtkRenderer::ComputeVisiblePropBounds( float allBounds[6] )
{
  vtkProp    *prop;
  float      *bounds;
  int        nothingVisible=1;

  allBounds[0] = allBounds[2] = allBounds[4] = VTK_LARGE_FLOAT;
  allBounds[1] = allBounds[3] = allBounds[5] = -VTK_LARGE_FLOAT;
  
  // loop through all props
  for (this->Props->InitTraversal(); (prop = this->Props->GetNextProp()); )
    {
    // if it's invisible, or has no geometry, we can skip the rest 
    if ( prop->GetVisibility() )
      {
      bounds = prop->GetBounds();
      // make sure we haven't got bogus bounds
      if ( bounds != NULL &&
           bounds[0] > -VTK_LARGE_FLOAT && bounds[1] < VTK_LARGE_FLOAT &&
           bounds[2] > -VTK_LARGE_FLOAT && bounds[3] < VTK_LARGE_FLOAT &&
           bounds[4] > -VTK_LARGE_FLOAT && bounds[5] < VTK_LARGE_FLOAT )
        {
        nothingVisible = 0;

        if (bounds[0] < allBounds[0])
          {
          allBounds[0] = bounds[0]; 
          }
        if (bounds[1] > allBounds[1])
          {
          allBounds[1] = bounds[1]; 
          }
        if (bounds[2] < allBounds[2])
          {
          allBounds[2] = bounds[2]; 
          }
        if (bounds[3] > allBounds[3])
          {
          allBounds[3] = bounds[3]; 
          }
        if (bounds[4] < allBounds[4])
          {
          allBounds[4] = bounds[4]; 
          }
        if (bounds[5] > allBounds[5])
          {
          allBounds[5] = bounds[5]; 
          }
        }//not bogus
      }
    }
  
  if ( nothingVisible )
    {
    vtkDebugMacro(<< "Can't compute bounds, no 3D props are visible");
    return;
    }
}

// Automatically set up the camera based on the visible actors.
// The camera will reposition itself to view the center point of the actors,
// and move along its initial view plane normal (i.e., vector defined from 
// camera position to focal point) so that all of the actors can be seen.
void vtkRenderer::ResetCamera()
{
  float      allBounds[6];

  this->ComputeVisiblePropBounds( allBounds );

  if ( allBounds[0] == VTK_LARGE_FLOAT )
    {
    vtkErrorMacro( << "Cannot reset camera!" );
    return;
    }

  this->ResetCamera(allBounds);
}

// Automatically set the clipping range of the camera based on the
// visible actors
void vtkRenderer::ResetCameraClippingRange()
{
  float      allBounds[6];

  this->ComputeVisiblePropBounds( allBounds );

  if ( allBounds[0] == VTK_LARGE_FLOAT )
    {
    vtkErrorMacro( << "Cannot reset camera!" );
    return;
    }

  this->ResetCameraClippingRange(allBounds);
}


// Automatically set up the camera based on a specified bounding box
// (xmin,xmax, ymin,ymax, zmin,zmax). Camera will reposition itself so
// that its focal point is the center of the bounding box, and adjust its
// distance and position to preserve its initial view plane normal 
// (i.e., vector defined from camera position to focal point). Note: is 
// the view plane is parallel to the view up axis, the view up axis will
// be reset to one of the three coordinate axes.
void vtkRenderer::ResetCamera(float bounds[6])
{
  float center[3];
  float distance;
  float width;
  double vn[3], *vup;
  
  this->GetActiveCamera();
  if ( this->ActiveCamera != NULL )
    {
    this->ActiveCamera->GetViewPlaneNormal(vn);
    }
  else
    {
    vtkErrorMacro(<< "Trying to reset non-existant camera");
    return;
    }

  center[0] = (bounds[0] + bounds[1])/2.0;
  center[1] = (bounds[2] + bounds[3])/2.0;
  center[2] = (bounds[4] + bounds[5])/2.0;

  width = bounds[3] - bounds[2];
  if (width < (bounds[1] - bounds[0]))
    {
    width = bounds[1] - bounds[0];
    }
  distance = 
    0.8*width/tan(this->ActiveCamera->GetViewAngle()*vtkMath::Pi()/360.0);
  distance = distance + (bounds[5] - bounds[4])/2.0;

  // check view-up vector against view plane normal
  vup = this->ActiveCamera->GetViewUp();
  if ( fabs(vtkMath::Dot(vup,vn)) > 0.999 )
    {
    vtkWarningMacro(<<"Resetting view-up since view plane normal is parallel");
    this->ActiveCamera->SetViewUp(-vup[2], vup[0], vup[1]);
    }

  // update the camera
  this->ActiveCamera->SetFocalPoint(center[0],center[1],center[2]);
  this->ActiveCamera->SetPosition(center[0]+distance*vn[0],
				  center[1]+distance*vn[1],
				  center[2]+distance*vn[2]);

  this->ResetCameraClippingRange( bounds );

  // setup default parallel scale
  this->ActiveCamera->SetParallelScale(width);
}
  
// Alternative version of ResetCamera(bounds[6]);
void vtkRenderer::ResetCamera(float xmin, float xmax, float ymin, float ymax, 
			      float zmin, float zmax)
{
  float bounds[6];

  bounds[0] = xmin;
  bounds[1] = xmax;
  bounds[2] = ymin;
  bounds[3] = ymax;
  bounds[4] = zmin;
  bounds[5] = zmax;

  this->ResetCamera(bounds);
}

// Reset the camera clipping range to include this entire bounding box
void vtkRenderer::ResetCameraClippingRange( float bounds[6] )
{
  double vn[3], position[3], a, b, c, d;
  double centerdist, diagdist;
  double range[2];

  this->GetActiveCamera();
  if ( this->ActiveCamera == NULL )
    {
    vtkErrorMacro(<< "Trying to reset clipping range of non-existant camera");
    return;
    }
  
  this->ActiveCamera->GetViewPlaneNormal(vn);
  this->ActiveCamera->GetPosition(position);
  a = -vn[0];
  b = -vn[1];
  c = -vn[2];
  d = -(a*position[0] + b*position[1] + c*position[2]);

  diagdist = 
    sqrt( (bounds[0] - bounds[1]) * (bounds[0] - bounds[1]) +
	  (bounds[2] - bounds[3]) * (bounds[2] - bounds[3]) +
	  (bounds[4] - bounds[5]) * (bounds[4] - bounds[5]) );
  
  centerdist = 
    a*(bounds[0]+bounds[1])/2.0 + 
    b*(bounds[2]+bounds[3])/2.0 + 
    c*(bounds[4]+bounds[5])/2.0 + 
    d;

  range[0] = centerdist - 0.5*diagdist;
  range[1] = centerdist + 0.5*diagdist;

  range[0] = (range[0] < 0.01)?(0.01):(range[0]);
  range[1] = (range[1] < range[0])?(range[0] + 0.1):(range[1]);

  this->ActiveCamera->SetClippingRange( range );
}

// Alternative version of ResetCameraClippingRange(bounds[6]);
void vtkRenderer::ResetCameraClippingRange(float xmin, float xmax, 
					   float ymin, float ymax, 
					   float zmin, float zmax)
{
  float bounds[6];

  bounds[0] = xmin;
  bounds[1] = xmax;
  bounds[2] = ymin;
  bounds[3] = ymax;
  bounds[4] = zmin;
  bounds[5] = zmax;

  this->ResetCameraClippingRange(bounds);
}

// Specify the rendering window in which to draw. This is automatically set
// when the renderer is created by MakeRenderer.  The user probably
// shouldn't ever need to call this method.
// no reference counting!
void vtkRenderer::SetRenderWindow(vtkRenderWindow *renwin)
{
  vtkProp *aProp;
  
  if (renwin != this->RenderWindow)
    {
    // This renderer is be dis-associated with its previous render window.
    // this information needs to be passed to the renderer's actors and
    // volumes so they can release and render window specific (or graphics
    // context specific) information (such as display lists and texture ids)
    this->Props->InitTraversal();
    for ( aProp = this->Props->GetNextProp();
	  aProp != NULL;
	  aProp = this->Props->GetNextProp() )
      {
      aProp->ReleaseGraphicsResources(this->RenderWindow);
      }
    // what about lights?
    // what about cullers?
    
    }
  this->VTKWindow = renwin;
  this->RenderWindow = renwin;
}

// Given a pixel location, return the Z value
float vtkRenderer::GetZ (int x, int y)
{
  float *zPtr;
  float z;

  zPtr = this->RenderWindow->GetZbufferData (x, y, x, y);
  if (zPtr)
    {
    z = *zPtr;
    delete [] zPtr;
    }
  else
    {
    z = 1.0;
    }
  return z;
}


// Convert view point coordinates to world coordinates.
void vtkRenderer::ViewToWorld()
{
  vtkMatrix4x4 *mat = vtkMatrix4x4::New();
  float result[4];

  // get the perspective transformation from the active camera 
  mat->DeepCopy(
	this->ActiveCamera->GetCompositePerspectiveTransformMatrix(1,0,1));
  
  // use the inverse matrix 
  mat->Invert();
 
  // Transform point to world coordinates 
  result[0] = this->ViewPoint[0];
  result[1] = this->ViewPoint[1];
  result[2] = this->ViewPoint[2];
  result[3] = 1.0;

  mat->Transpose();
  mat->PointMultiply(result,result);
  
  // Get the transformed vector & set WorldPoint 
  // while we are at it try to keep w at one
  if (result[3])
    {
    result[0] /= result[3];
    result[1] /= result[3];
    result[2] /= result[3];
    result[3] = 1;
    }
  
  this->SetWorldPoint(result);
  mat->Delete();
}

void vtkRenderer::ViewToWorld(float &x, float &y, float &z)
{
  vtkMatrix4x4 *mat = vtkMatrix4x4::New();
  float result[4];

  // get the perspective transformation from the active camera 
  mat->DeepCopy(
    this->ActiveCamera->GetCompositePerspectiveTransformMatrix(1,0,1));
  
  // use the inverse matrix 
  mat->Invert();
 
  // Transform point to world coordinates 
  result[0] = x;
  result[1] = y;
  result[2] = z;
  result[3] = 1.0;

  mat->Transpose();
  mat->PointMultiply(result,result);
  
  // Get the transformed vector & set WorldPoint 
  // while we are at it try to keep w at one
  if (result[3])
    {
    x = result[0] / result[3];
    y = result[1] / result[3];
    z = result[2] / result[3];
    }
  mat->Delete();
}

// Convert world point coordinates to view coordinates.
void vtkRenderer::WorldToView()
{
  vtkMatrix4x4 *matrix = vtkMatrix4x4::New();
  float     view[4];
  float     *world;

  // get the perspective transformation from the active camera 
  matrix->DeepCopy(
	   this->ActiveCamera->GetCompositePerspectiveTransformMatrix(1,0,1));

  world = this->WorldPoint;
  view[0] = world[0]*matrix->Element[0][0] + world[1]*matrix->Element[0][1] +
    world[2]*matrix->Element[0][2] + world[3]*matrix->Element[0][3];
  view[1] = world[0]*matrix->Element[1][0] + world[1]*matrix->Element[1][1] +
    world[2]*matrix->Element[1][2] + world[3]*matrix->Element[1][3];
  view[2] = world[0]*matrix->Element[2][0] + world[1]*matrix->Element[2][1] +
    world[2]*matrix->Element[2][2] + world[3]*matrix->Element[2][3];
  view[3] = world[0]*matrix->Element[3][0] + world[1]*matrix->Element[3][1] +
    world[2]*matrix->Element[3][2] + world[3]*matrix->Element[3][3];

  if (view[3] != 0.0)
    {
    this->SetViewPoint(view[0]/view[3],
		       view[1]/view[3],
		       view[2]/view[3]);
    }
  matrix->Delete();
}

// Convert world point coordinates to view coordinates.
void vtkRenderer::WorldToView(float &x, float &y, float &z)
{
  vtkMatrix4x4 *matrix = vtkMatrix4x4::New();
  float     view[4];

  // get the perspective transformation from the active camera
  matrix->DeepCopy(
    this->ActiveCamera->GetCompositePerspectiveTransformMatrix(1,0,1));

  view[0] = x*matrix->Element[0][0] + y*matrix->Element[0][1] +
    z*matrix->Element[0][2] + matrix->Element[0][3];
  view[1] = x*matrix->Element[1][0] + y*matrix->Element[1][1] +
    z*matrix->Element[1][2] + matrix->Element[1][3];
  view[2] = x*matrix->Element[2][0] + y*matrix->Element[2][1] +
    z*matrix->Element[2][2] + matrix->Element[2][3];
  view[3] = x*matrix->Element[3][0] + y*matrix->Element[3][1] +
    z*matrix->Element[3][2] + matrix->Element[3][3];

  if (view[3] != 0.0)
    {
    x = view[0]/view[3];
    y = view[1]/view[3];
    z = view[2]/view[3];
    }
  matrix->Delete();
}

void vtkRenderer::PrintSelf(ostream& os, vtkIndent indent)
{
  this->vtkViewport::PrintSelf(os,indent);

  os << indent << "Ambient: (" << this->Ambient[0] << ", " 
     << this->Ambient[1] << ", " << this->Ambient[2] << ")\n";

  os << indent << "BackingStore: " << (this->BackingStore ? "On\n":"Off\n");
  os << indent << "DisplayPoint: ("  << this->DisplayPoint[0] << ", " 
    << this->DisplayPoint[1] << ", " << this->DisplayPoint[2] << ")\n";
  os << indent << "Lights:\n";
  this->Lights->PrintSelf(os,indent.GetNextIndent());

  os << indent << "ViewPoint: (" << this->ViewPoint[0] << ", " 
    << this->ViewPoint[1] << ", " << this->ViewPoint[2] << ")\n";

  os << indent << "Two-sided Lighting: " 
     << (this->TwoSidedLighting ? "On\n" : "Off\n");

  if ( this->RayCaster )
    {
    os << indent << "Ray Caster: " << this->RayCaster << "\n";
    }
  else
    {
    os << indent << "Ray Caster: (none)\n";
    }

  os << indent << "Allocated Render Time: " << this->AllocatedRenderTime
     << "\n";

  os << indent << "Last Time To Render (Seconds): " 
     << this->LastRenderTimeInSeconds << endl;

  // I don't want to print this since it is used just internally
  // os << indent << this->NumberOfPropsRenderedAsGeometry;

}

int vtkRenderer::VisibleActorCount()
{
  vtkProp *aProp;
  int count = 0;

  // loop through Props
  for (this->Props->InitTraversal();
       (aProp = this->Props->GetNextProp()); )
    {
    if (aProp->GetVisibility())
      {
      count++;
      }
    }
  return count;
}

int vtkRenderer::VisibleVolumeCount()
{
  int count = 0;
  vtkProp *aProp;

  // loop through volumes
  for (this->Props->InitTraversal(); 
	(aProp = this->Props->GetNextProp()); )
    {
    if (aProp->GetVisibility())
      {
      count++;
      }
    }
  return count;
}

// We need to override the unregister method because the raycaster
// is registered by the renderer and the renderer is registered by
// raycaster. If we are down to just two references on the renderer
// (from itself and the raycaster) then delete it, and the raycaster.
void vtkRenderer::UnRegister(vtkObject *o)
{
  if (this->RayCaster != NULL && this->RayCaster->GetRenderer() == this &&
      this->GetReferenceCount() == 2 )
    {
    vtkRayCaster *temp = this->RayCaster;
    this->RayCaster = NULL;    
    temp->Delete();
    }

  this->vtkObject::UnRegister(o);
}



unsigned long int vtkRenderer::GetMTime()
{
  unsigned long mTime=this-> vtkViewport::GetMTime();
  unsigned long time;

  if ( this-> RayCaster != NULL )
    {
    time = this->RayCaster ->GetMTime();
    mTime = ( time > mTime ? time : mTime );
    }
  if ( this->ActiveCamera != NULL )
    {
    time = this->ActiveCamera ->GetMTime();
    mTime = ( time > mTime ? time : mTime );
    }
  if ( this->CreatedLight != NULL )
    {
    time = this->CreatedLight ->GetMTime();
    mTime = ( time > mTime ? time : mTime );
    }

  return mTime;
}

