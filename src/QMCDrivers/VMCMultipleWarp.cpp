//////////////////////////////////////////////////////////////////
// (c) Copyright 2003- by Jeongnim Kim and Simone Chiesa
//////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////
//   Jeongnim Kim
//   National Center for Supercomputing Applications &
//   Materials Computation Center
//   University of Illinois, Urbana-Champaign
//   Urbana, IL 61801
//   e-mail: jnkim@ncsa.uiuc.edu
//   Tel:    217-244-6319 (NCSA) 217-333-3324 (MCC)
//
// Supported by 
//   National Center for Supercomputing Applications, UIUC
//   Materials Computation Center, UIUC
//   Department of Physics, Ohio State University
//   Ohio Supercomputer Center
//////////////////////////////////////////////////////////////////
// -*- C++ -*-
#include "QMCDrivers/VMCMultipleWarp.h"
#include "Utilities/OhmmsInfo.h"
#include "Particle/MCWalkerConfiguration.h"
#include "Particle/HDFWalkerIO.h"
#include "ParticleBase/ParticleUtility.h"
#include "ParticleBase/RandomSeqGenerator.h"
#include "Message/Communicate.h"
#include "Utilities/Clock.h"
#include "Estimators/MultipleEnergyEstimator.h"
#include "Particle/DistanceTable.h"
#include "QMCApp/ParticleSetPool.h"

namespace qmcplusplus { 


  /// Constructor.
  VMCMultipleWarp::VMCMultipleWarp(MCWalkerConfiguration& w, TrialWaveFunction& psi, QMCHamiltonian& h,
      ParticleSetPool& ptclPool):
    QMCDriver(w,psi,h), PtclPool(ptclPool), multiEstimator(0) { 
    RootName = "vmc-warp";
    QMCType ="vmc-warp";
    equilBlocks=-1;
    m_param.add(equilBlocks,"equilBlocks","int");

    QMCDriverMode.set(QMC_MULTIPLE,1);
    JACOBIAN=w.addProperty("Jacobian");
    //Add the primary h and psi, extra H and Psi pairs will be added by QMCMain
    add_H_and_Psi(&h,&psi);

  }

  /** allocate internal data here before run() is called
   * @author SIMONE
   *
   * See QMCDriver::process
   */
  bool VMCMultipleWarp::put(xmlNodePtr q){

    //qmcsystem
    vector<DistanceTableData*> dtableList;
    string target_name(W.getName());
    xmlNodePtr cur=q->children;
    while(cur != NULL) {
      string cname((const char*)(cur->name));
      if(cname == "qmcsystem") {
        string source_name((const char*)xmlGetProp(cur,(const xmlChar*)"source"));
	dtableList.push_back(DistanceTable::getTable(source_name.c_str(),target_name.c_str()));
      }
      cur=cur->next;
    }

    nptcl=W.R.size();
    nPsi=Psi1.size();	

    PtclWarp.initialize(dtableList);

    logpsi.resize(nPsi);
    sumratio.resize(nPsi);	
    invsumratio.resize(nPsi);	
    Jacobian.resize(nPsi);	

    Norm.resize(nPsi);
    if(branchEngine->LogNorm.size()==0)branchEngine->LogNorm.resize(nPsi);
    if(equilBlocks>0){
      for(int ipsi=0; ipsi<nPsi; ipsi++)branchEngine->LogNorm[ipsi]=0.e0;
    }


    for(int ipsi=0; ipsi<nPsi; ipsi++) 
      H1[ipsi]->add2WalkerProperty(W);

    if(Estimators == 0) {
      Estimators = new ScalarEstimatorManager(H);
      multiEstimator = new MultipleEnergyEstimator(H,nPsi);
      Estimators->add(multiEstimator,"elocal");
    }

    LOGMSG("Number of H and Psi " << nPsi)

    H1[0]->setPrimary(true);
    for(int ipsi=1; ipsi<nPsi; ipsi++) {
      H1[ipsi]->setPrimary(false);
    }

    if(WW.empty()){
      WW.push_back(&W);
      char newname[128];
      for(int ipsi=1; ipsi<nPsi; ipsi++){
	sprintf(newname,"%s%d", W.getName().c_str(),ipsi);
        ParticleSet* pclone=PtclPool.getParticleSet(newname);
        if(pclone == 0) {
          app_log() << "  Cloning particle set in VMCMultipleWarp " << newname << endl;
          pclone=new ParticleSet(W);
          pclone->setName(newname);
          PtclPool.addParticleSet(pclone);
        } else {
          app_log() << "  Cloned particle exists " << newname << endl;
        }
	//Correct copy constructor????????
	WW.push_back(pclone);
	WW[ipsi]=pclone;
	Psi1[ipsi]->resetTargetParticleSet(*WW[ipsi]);
        H1[ipsi]->resetTargetParticleSet(*WW[ipsi]);
	//Psi1[ipsi]->resetTargetParticleSet(W);
        //H1[ipsi]->resetTargetParticleSet(W
        //WW[ipsi]->setUpdateMode(MCWalkerConfiguration::Update_Particle);
      }
    }

    return true;
  }
  
  /** Run the VMCMultipleWarp algorithm.
   *
   * Similar to VMC::run 
   */
  bool VMCMultipleWarp::run() { 
    
    int JACCOL=Estimators->addColumn("LogJacob");
      
    Estimators->reportHeader(AppendRun);

    bool require_register=false;

    //Check if we need to update the norm of the wave functions
    vector<RealType> tmpNorm(nPsi);
    if(equilBlocks > 0){
      for(int ipsi=0; ipsi< nPsi; ipsi++){
        Norm[ipsi]=1.0;
        tmpNorm[ipsi]=0.0;
      }
    }else{
      for(int ipsi=0; ipsi< nPsi; ipsi++) Norm[ipsi]=std::exp(branchEngine->LogNorm[ipsi]);
    }

    //this is where the first values are evaulated
    multiEstimator->initialize(W,WW,PtclWarp,H1,Psi1,Tau,Norm,require_register);
    
    Estimators->reset();

    IndexType block = 0;
    
    Pooma::Clock timer;
    
    double wh=0.0;
    IndexType nAcceptTot = 0;
    IndexType nRejectTot = 0;
    
    branchEngine->LogJacobRef=0.e0;
    do {
      IndexType step = 0;
      nAccept = 0; nReject=0;

      Estimators->startBlock();
      RealType Jacblk=0.e0;
      do {
        advanceWalkerByWalker();
        step++;CurrentStep++;
        Estimators->accumulate(W);
        Jacblk+=std::log(fabs((*W.begin())->Properties(1,JACOBIAN)));
      } while(step<nSteps);
      
      //Modify Norm. 
      if(block < equilBlocks){
        for(int ipsi=0; ipsi< nPsi; ipsi++){
          tmpNorm[ipsi]+=multiEstimator->esum(ipsi,MultipleEnergyEstimator::WEIGHT_INDEX);
        }
        if(block==(equilBlocks-1) || block==(nBlocks-1)){
          RealType SumNorm(0.e0);
          for(int ipsi=0; ipsi< nPsi; ipsi++) SumNorm+=tmpNorm[ipsi];
          for(int ipsi=0; ipsi< nPsi; ipsi++){
            Norm[ipsi]=tmpNorm[ipsi]/SumNorm;
            branchEngine->LogNorm[ipsi]=std::log(Norm[ipsi]);
          }
          cout << "BranchEngine is updated " << branchEngine->LogNorm[0] << " " << branchEngine->LogNorm[1] << endl;
        }
      }

      Estimators->stopBlock(nAccept/static_cast<RealType>(nAccept+nReject));
      RealType AveJacobLog=Jacblk/static_cast<RealType>(nSteps);
      Estimators->setColumn(JACCOL,AveJacobLog);


      nAcceptTot += nAccept;
      nRejectTot += nReject;

      branchEngine->accumulate(Estimators->average(0),1.0);
      branchEngine->LogJacobRef+=AveJacobLog;

      nAccept = 0; nReject = 0;
      block++;

      //record the current configuration
      recordBlock(block);

    } while(block<nBlocks);
    branchEngine->LogJacobRef/=static_cast<RealType>(nBlocks);

    //Normalize norms :-)

   // do {
   //   IndexType step = 0;
   //   timer.start();
   //   nAccept = 0; nReject=0;
   //   do {
   //     advanceWalkerByWalker();
   //     step++;CurrentStep++;
   //     Estimators->accumulate(W);
   //   } while(step<nSteps);

   //   timer.stop();
   //   nAcceptTot += nAccept; nRejectTot += nReject;
   //   Estimators->flush();
   //   RealType TotalConfig=static_cast<RealType>(nAccept+nReject);		
   //   Estimators->setColumn(AcceptIndex,nAccept/TotalConfig);		

   //   Estimators->report(CurrentStep);
   //   
   //   LogOut->getStream() << "Block " << block << " " << timer.cpu_time() << endl;

   //   //HDFWalkerOutput WO(RootName,block&&appendwalker, block);
   //   //WO.get(W);

   //   branchEngine->accumulate(Estimators->average(0),1.0);

   //   nAccept = 0; nReject = 0;
   //   block++;

   //   //record the current configuration
   //   recordWalkerConfigurations(block);

   // } while(block<nBlocks);

    
    app_log()
      << "Ratio = " 
      << static_cast<double>(nAcceptTot)/static_cast<double>(nAcceptTot+nRejectTot)
      << endl;
    
    /*
    int nconf= appendwalker ? block:1;
    HDFWalkerOutput WOextra(RootName,true,nconf);
    WOextra.write(*branchEngine);
    
    Estimators->finalize();

    return true;*/

    //finalize a qmc section
    return finalize(block);
  }

  /**  Advance all the walkers one timstep. 
   */
  void 
  VMCMultipleWarp::advanceWalkerByWalker() {
    
    m_oneover2tau = 0.5/Tau;
    m_sqrttau = sqrt(Tau);
    
    //MCWalkerConfiguration::PropertyContainer_t Properties;
    
    MCWalkerConfiguration::iterator it(W.begin()); 
    MCWalkerConfiguration::iterator it_end(W.end()); 
    int iwlk(0); 
    int nPsi_minus_one(nPsi-1);

    while(it != it_end) {

      MCWalkerConfiguration::Walker_t &thisWalker(**it);

      //create a 3N-Dimensional Gaussian with variance=1
      makeGaussRandom(deltaR);
     
      W.R = m_sqrttau*deltaR + thisWalker.R + thisWalker.Drift;
      
      //Evaluate Psi and graidients and laplacians
      //\f$\sum_i \ln(|psi_i|)\f$ and catching the sign separately
      //WW[0]->R=W.R;

      W.update();

      for(int ipsi=0; ipsi<nPsi; ipsi++) Jacobian[ipsi]=1.e0;
      for(int iptcl=0; iptcl< nptcl; iptcl++){
	PtclWarp.warp_one(iptcl,0);
	//Save particle position
	for(int ipsi=0; ipsi<nPsi; ipsi++){
	  WW[ipsi]->R[iptcl]=W.R[iptcl]+PtclWarp.get_displacement(iptcl,ipsi);
	  Jacobian[ipsi]*=PtclWarp.get_Jacobian(iptcl,ipsi);
	}
      }

      /*for(int i=0; i<nptcl; i++){
        for(int ipsi=0; ipsi<nPsi; ipsi++){
          cout << ipsi << " " << WW[ipsi]->R[i] << "         ";
	}
        cout << endl;
      }
      cout << endl;*/

      for(int ipsi=0; ipsi< nPsi;ipsi++) {
        if(ipsi) WW[ipsi]->update();
        //SIMONE SIMONE SIMONE SIMONE
	logpsi[ipsi]=Psi1[ipsi]->evaluateLog(*WW[ipsi]);
        //cout << "LOGPSI " << ipsi << " "<< logpsi[ipsi] << endl;
	//Redundant???
        Psi1[ipsi]->L=WW[ipsi]->L;
        Psi1[ipsi]->G=WW[ipsi]->G;
	sumratio[ipsi]=1.0;
      }

      // Compute the sum over j of Psi^2[j]/Psi^2[i] for each i
      for(int ipsi=0; ipsi< nPsi_minus_one; ipsi++) {
	for(int jpsi=ipsi+1; jpsi< nPsi; jpsi++){
	  RealType ratioij=Norm[ipsi]/Norm[jpsi]*std::exp(2.0*(logpsi[jpsi]-logpsi[ipsi]));
	  ratioij*=(Jacobian[jpsi]/Jacobian[ipsi]);
	  sumratio[ipsi] += ratioij;
	  sumratio[jpsi] += 1.0/ratioij;
	}
      }  

      for(int ipsi=0; ipsi< nPsi; ipsi++)			  
	invsumratio[ipsi]=1.0/sumratio[ipsi];			  

     // cout << invsumratio[0] << " " << invsumratio[1] << endl;

      // Only these properties need to be updated
      // Using the sum of the ratio Psi^2[j]/Psi^2[iwref]
      // because these are number of order 1. Potentially
      // the sum of Psi^2[j] can get very big
      //Properties(LOGPSI) =logpsi[0];
      //Properties(SUMRATIO) = sumratio[0];
 
      RealType logGf = -0.5*Dot(deltaR,deltaR);
      ValueType scale = Tau; // ((-1.0+sqrt(1.0+2.0*Tau*vsq))/vsq);

      //accumulate the weighted drift
      QMCTraits::PosType WarpDrift;
      RealType denom(0.e0),wgtpsi;
      drift=0.e0; 
      for(int ipsi=0; ipsi<nPsi; ipsi++) {
        wgtpsi=1.e0/sumratio[ipsi];
        denom += wgtpsi;
        for(int iptcl=0; iptcl< nptcl; iptcl++){
          WarpDrift=dot(  Psi1[ipsi]->G[iptcl],PtclWarp.get_Jacob_matrix(iptcl,ipsi)  )
            +5.0e-1*PtclWarp.get_grad_ln_Jacob(iptcl,ipsi) ;
          drift[iptcl] += (wgtpsi*WarpDrift);
        }
      }
      drift *= (scale/denom);

      deltaR = thisWalker.R - W.R - drift;
      RealType logGb = -m_oneover2tau*Dot(deltaR,deltaR);
      
      //Original
      //RealType g = Properties(SUMRATIO)/thisWalker.Properties(SUMRATIO)*   		
      //	std::exp(logGb-logGf+2.0*(Properties(LOGPSI)-thisWalker.Properties(LOGPSI)));	
      //Reuse Multiplicity to store the sumratio[0]
      RealType g = sumratio[0]/thisWalker.Multiplicity*   		
       	std::exp(logGb-logGf+2.0*(logpsi[0]-thisWalker.Properties(LOGPSI)));	

      //cout << "ACCPROB " << g << endl;

      //g=1.0;

      if(Random() > g) {
	thisWalker.Age++;     
	++nReject; 
  //      cout << " REJECTED " << endl;
      } else {
	thisWalker.Age=0;
        thisWalker.Multiplicity=sumratio[0];
	thisWalker.R = W.R;
	thisWalker.Drift = drift;
	for(int ipsi=0; ipsi<nPsi; ipsi++){ 		            
          WW[ipsi]->L=Psi1[ipsi]->L; //NECESSARY??????? 
          WW[ipsi]->G=Psi1[ipsi]->G;
	  RealType et = H1[ipsi]->evaluate(*WW[ipsi]);
          //cout << " ENERGY " <<  et << endl ;
          thisWalker.Properties(ipsi,LOGPSI)=logpsi[ipsi];
          thisWalker.Properties(ipsi,SIGN) =Psi1[ipsi]->getSign();
          thisWalker.Properties(ipsi,UMBRELLAWEIGHT)=invsumratio[ipsi];
          thisWalker.Properties(ipsi,LOCALENERGY)=et;
          thisWalker.Properties(ipsi,JACOBIAN)=Jacobian[ipsi];
          //multiEstimator->updateSample(iwlk,ipsi,et,invsumratio[ipsi]); 
          H1[ipsi]->saveProperty(thisWalker.getPropertyBase(ipsi));
	}

	++nAccept;
      }
      ++it; 
      ++iwlk;
    }
    //cout << endl;
  }
}

/***************************************************************************
 * $RCSfile$   $Author$
 * $Revision$   $Date$
 * $Id$ 
 ***************************************************************************/
