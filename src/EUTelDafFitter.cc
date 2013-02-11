// Author Havard Gjersdal, UiO(haavagj@fys.uio.no)
/*!
 * This is a track fitting processor for the Eutelescope package. 
 *
 * It preforms track finding and fitting on a supplied hit collection.
 *
 * The track finder works by propagating all hits to plane 0, currently assuming straight
 * line fits, then running a cluster finder. Hit clusters above some set value are considered
 * track candidates.
 *
 * This track candidate is then fitted using a implementation of a Deterministic Annealing
 * Filter (DAF), that in short is a Kalman Filter running iteratively over a set of weighted
 * measurements, reweighing the measurements after each fit based on the residuals and a
 * supplied chi2 cut off.
 *
 * This package uses the Eigen library for linear algebra. This package is very quick when
 * compiled properly, but very slow when compiled for debugging. Make sure to compile
 * properly before running productions.
 *
 * Running 'cmake -i' inside the build folder, and then when it asks
 * Variable Name: CMAKE_CXX_FLAGS_RELEASE
 * Description: Flags used by the compiler during release builds (/MD /Ob1 /Oi /Ot /Oy /Gs will produce slightly less optimized but smaller files).                          
 *
 * enter:
 * New Value (Enter to keep current value): -O3 -msse2 -ftree-vectorize -DNDEBUG
 *
 * When it asks
 * Variable Name: CMAKE_BUILD_TYPE
 * enter:
 * New Value (Enter to keep current value): Release
 *
 * If youc cpu supports it, you could try -msse4 or -msse3 aswell.
 */

// built only if GEAR and MARLINUTIL are used
#if defined(USE_GEAR)
// eutelescope includes ".h"
#include "EUTelDafFitter.h"
#include "EUTelRunHeaderImpl.h"
#include "EUTelEventImpl.h"
#include "EUTELESCOPE.h"
#include "EUTelVirtualCluster.h"
#include "EUTelExceptions.h"

// marlin includes ".h"
#include "marlin/Processor.h"
#include "marlin/Global.h"
#include "marlin/Exceptions.h"
#include "marlin/AIDAProcessor.h"

// gear includes <.h>
#include <gear/GearMgr.h>
#include <gear/SiPlanesParameters.h>

// aida includes <.h>
#if defined(USE_AIDA) || defined(MARLIN_USE_AIDA)
#include <marlin/AIDAProcessor.h>
#include <AIDA/IHistogramFactory.h>
#include <AIDA/IHistogram1D.h>
#include <AIDA/IProfile1D.h>
#include <AIDA/ITree.h>
#endif

// lcio includes <.h>
#include <IO/LCWriter.h>
#include <UTIL/LCTime.h>
#include <EVENT/LCCollection.h>
#include <EVENT/LCEvent.h>
#include <IMPL/LCCollectionVec.h>
#include <IMPL/TrackerHitImpl.h>
#include <IMPL/TrackImpl.h>
#include <IMPL/LCFlagImpl.h>
#include <Exceptions.h>

// system includes <>
#include <string>
#include <vector>
#include <algorithm>
#include <map>
#include <memory>
#include <cmath>
#include <iostream>
#include <fstream>

using namespace std;
using namespace lcio;
using namespace marlin;
using namespace eutelescope;


EUTelDafFitter::EUTelDafFitter () : EUTelDafBase("EUTelDafFitter"){
  //Child spesific params and description
  dafParams();
}

void EUTelDafFitter::dafParams(){
  //Parameters for the fitter
  _description = "This processor preforms track reconstruction. The tracks are as final track fit for analysis.";

  //Tracker system options
  registerOptionalParameter("AddToLCIO", "Should plots be made and filled?", _addToLCIO, static_cast<bool>(true));
  registerOptionalParameter("FitDuts","Set this to true if you want DUTs to be included in the track fit", _fitDuts, static_cast<bool>(false)); 
  //Track fitter options
  registerOutputCollection(LCIO::TRACK,"TrackCollectionName", "Collection name for fitted tracks", _trackCollectionName, string ("fittracks"));
}

void EUTelDafFitter::dafInit() {
  //Enable DUT fitting if configurations demands it
  if(_fitDuts){
    for( size_t ii = 0; ii< _system.planes.size(); ii++){
      if( find(_dutPlanes.begin(), _dutPlanes.end(), _system.planes.at(ii).getSensorID()) != _dutPlanes.end()){ 
	_system.planes.at(ii).include(); 
      }
    }
  }
}

void EUTelDafFitter::dafEvent (LCEvent * event) {
  //Find, fit and save event tracks

  //Prepare track collection
  if(_addToLCIO){
    // Define output track and hit collections
    _fittrackvec = new LCCollectionVec(LCIO::TRACK);
    _fitpointvec = new LCCollectionVec(LCIO::TRACKERHIT);
    // Set flag for storing track hits in track collection
    LCFlagImpl flag(_fittrackvec->getFlag());
    flag.setBit( LCIO::TRBIT_HITS );
    _fittrackvec->setFlag(flag.getFlag());
  }
  
  //Check found tracks
  for(size_t ii = 0; ii < _system.getNtracks(); ii++ ){
    //run track fitte
    _nClusters++;
    _system.weightToIndex(_system.tracks.at(ii));
    _system.fitPlanesInfoDaf(_system.tracks.at(ii));
    //Check resids, intime, angles
    if(not checkTrack( _system.tracks.at(ii))) { continue;};
    int inTimeHits = checkInTime( _system.tracks.at(ii));
    if( inTimeHits < _nDutHits) { continue;}

    //Fill plots
    if(_histogramSwitch){ 
      fillPlots( _system.tracks.at(ii) ); 
      fillDetailPlots( _system.tracks.at(ii) ); 
    }
    //Dump to LCIO
    if( _addToLCIO) { addToLCIO(_system.tracks.at(ii)); }
    _nTracks++;
  }

 //Add track collection
  if(_addToLCIO){ 
    event->addCollection(_fittrackvec,_trackCollectionName); 
    event->addCollection(_fitpointvec, "fitpoints");
  }
}

void EUTelDafFitter::addToLCIO(daffitter::TrackCandidate<float, 4>* track){
  //Save track to LCIO

  TrackImpl * fittrack = new TrackImpl();
  // Impact parameters are useless and set to 0
  fittrack->setD0(0.);        // impact paramter of the track in (r-phi)
  fittrack->setZ0(0.);        // impact paramter of the track in (r-z)
  fittrack->setTanLambda(0.); // dip angle of the track at reference point

  daffitter::TrackEstimate<float, 4> * est = track->estimates.at(0);

  //No good way of storing the track angles, so
  fittrack->setOmega( est->getXdz() ); // Storing dxdz as Omega
  fittrack->setPhi( est->getYdz() );   // Storing dx/dy as phi

  fittrack->setChi2(track->chi2);
  fittrack->setNdf(int(track->ndof + 0.2f) );
  
  fittrack->setIsReferencePointPCA(false);
  float refpoint[3];
  
  for(size_t plane = 0; plane < _system.planes.size(); plane++){
    daffitter::FitPlane<float>& pl = _system.planes.at(plane);
    daffitter::TrackEstimate<float, 4> * estim = track->estimates.at( plane );
    TrackerHitImpl * fitpoint = new TrackerHitImpl();
    // Hit type 32 means a fitted hit
    fitpoint->setType(32);
    double pos[3];
    pos[0]= estim->getX() / 1000.0;
    pos[1]= estim->getY() / 1000.0;
    pos[2]= pl.getMeasZ() / 1000.0;
      
    fitpoint->setPosition(pos);
    // Covariance matrix of the fitted position
    // (stored as lower triangle matrix, i.e.  cov(xx),cov(y,x),cov(y,y) ).
    float cov[TRKHITNCOVMATRIX];
    cov[0]= estim->cov(0,0);
    cov[1]= estim->cov(0,1);
    cov[2]= estim->cov(1,1);
    //Error of z position of fit is unclear to me, this would be a systematic alignment
    //error. Set to 0 along with all covariances.
    cov[3]=cov[4]=cov[5]=0.;
    fitpoint->setCovMatrix(cov);
    _fitpointvec->push_back(fitpoint);
    fittrack->addHit(fitpoint);
    if(plane == 0){
      refpoint[0] = pos[0];
      refpoint[1] = pos[1];
      refpoint[2] = pos[2];
    }
    //Also add measurement point
    for(size_t mm = 0; mm < pl.meas.size(); mm++){
      if( pl.weights(mm) < 0.5f){ continue; }
      if( pl.isExcluded()) { continue; }
      TrackerHitImpl* meashit = static_cast<TrackerHitImpl*> ( _hitCollection->getElementAt( pl.meas.at(mm).getIden()) );
      fittrack->addHit(meashit);
    }
  }
  fittrack->setReferencePoint(refpoint);
  _fittrackvec->addElement(fittrack);
}

void EUTelDafFitter::dafEnd() {
}
#endif // USE_GEAR