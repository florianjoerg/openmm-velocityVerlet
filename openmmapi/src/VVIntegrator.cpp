/* -------------------------------------------------------------------------- *
 *                                   OpenMM                                   *
 * -------------------------------------------------------------------------- *
 * This is part of the OpenMM molecular simulation toolkit originating from   *
 * Simbios, the NIH National Center for Physics-Based Simulation of           *
 * Biological Structures at Stanford, funded under the NIH Roadmap for        *
 * Medical Research, grant U54 GM072970. See https://simtk.org.               *
 *                                                                            *
 * Portions copyright (c) 2008-2015 Stanford University and the Authors.      *
 * Authors: Peter Eastman                                                     *
 * Contributors:                                                              *
 *                                                                            *
 * Permission is hereby granted, free of charge, to any person obtaining a    *
 * copy of this software and associated documentation files (the "Software"), *
 * to deal in the Software without restriction, including without limitation  *
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,   *
 * and/or sell copies of the Software, and to permit persons to whom the      *
 * Software is furnished to do so, subject to the following conditions:       *
 *                                                                            *
 * The above copyright notice and this permission notice shall be included in *
 * all copies or substantial portions of the Software.                        *
 *                                                                            *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR *
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,   *
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL    *
 * THE AUTHORS, CONTRIBUTORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,    *
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR      *
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE  *
 * USE OR OTHER DEALINGS IN THE SOFTWARE.                                     *
 * -------------------------------------------------------------------------- */

#include "openmm/VVIntegrator.h"
#include "openmm/Context.h"
#include "openmm/OpenMMException.h"
#include "openmm/internal/AssertionUtilities.h"
#include "openmm/internal/ContextImpl.h"
#include "openmm/VVKernels.h"
#include <algorithm>
#include <vector>
#include "openmm/reference/SimTKOpenMMRealType.h"

using namespace OpenMM;
using std::string;
using std::vector;

VVIntegrator::VVIntegrator(double temperature, double frequency, double drudeTemperature,
                           double drudeFrequency, double stepSize,
                           int numNHChains, int loopsPerStep, bool useCOMTempGroup)
        : forcesAreValid(false) {
    setTemperature(temperature);
    setFrequency(frequency);
    setDrudeTemperature(drudeTemperature);
    setDrudeFrequency(drudeFrequency);
    setStepSize(stepSize);
    setNumNHChains(numNHChains);
    setLoopsPerStep(loopsPerStep);
    setUseCOMTempGroup(useCOMTempGroup);
    setConstraintTolerance(1e-5);
    setMaxDrudeDistance(0);
    setFriction(5.0);
    setDrudeFriction(20.0);
    setRandomNumberSeed(0);
    setMirrorLocation(0.0);
    setElectricField(0.0);
    setCosAcceleration(0.0);
    setDebugEnabled(false);
}

VVIntegrator::~VVIntegrator() {

}

int VVIntegrator::addImagePair(int image, int parent) {
    particlesImage.push_back(image);
    imagePairs.emplace_back(image, parent);
    return imagePairs.size();
}

double VVIntegrator::getResInvMass(int resid) const {
    ASSERT_VALID_INDEX(resid, residueInvMasses);
    return residueInvMasses[resid];
}

int VVIntegrator::getParticleResId(int particle) const {
    ASSERT_VALID_INDEX(particle, particleResId);
    return particleResId[particle];
}

void VVIntegrator::initialize(ContextImpl& contextRef) {
    if (owner != NULL && &contextRef.getOwner() != owner)
        throw OpenMMException("This Integrator is already bound to a context");

    const DrudeForce* force = NULL;
    const System& system = contextRef.getSystem();
    for (int i = 0; i < system.getNumForces(); i++) {
        if (dynamic_cast<const DrudeForce*>(&system.getForce(i)) != NULL) {
            if (force == NULL)
                force = dynamic_cast<const DrudeForce*>(&system.getForce(i));
            else
                throw OpenMMException("The System contains multiple DrudeForces");
        }
    }
    if (force == NULL && useCOMTempGroup)
        throw OpenMMException("Should not use COM temperature group for non-Drude model");

    // TODO Should consider the special case where a molecule contains only one real atom

    particleResId = std::vector<int>(system.getNumParticles(), -1);
    std::vector<std::vector<int> > molecules = contextRef.getMolecules();
    int numResidues = (int) molecules.size();
    for (int i = 0; i < numResidues; i++)
        for (int j = 0; j < (int) molecules[i].size(); j++)
            particleResId[molecules[i][j]] = i;

    residueMasses = std::vector<double>(numResidues, 0.0);
    for (int i = 0; i < system.getNumParticles(); i++)
        residueMasses[particleResId[i]] += system.getParticleMass(i);

    for (int i = 0; i < numResidues; i++)
        residueInvMasses.push_back(1.0/residueMasses[i]);
 
    // handle particles thermostated by Langevin dynamics
    for (int i = 0; i < system.getNumParticles(); i++) {
        if (!isParticleLD(i) && !isParticleImage(i)) {
            particlesNH.push_back(i);
            if (std::find(residuesNH.begin(), residuesNH.end(), getParticleResId(i)) == residuesNH.end()) {
                residuesNH.push_back(getParticleResId(i));
            }
        }
    }
    for (int i = 0; i < system.getNumParticles(); i++) {
        if (isParticleLD(i)
            && std::find(residuesNH.begin(), residuesNH.end(), getParticleResId(i)) != residuesNH.end()) {
            throw OpenMMException("NH and Langevin thermostat cannot be applied on the same molecule");
        }
    }

    // conflicts
    if (!particlesLD.empty() && cosAcceleration != 0)
        throw OpenMMException("Langevin thermostat and periodic perturbation shouldn't be used together");

    context = &contextRef;
    owner = &contextRef.getOwner();
    vvKernel = context->getPlatform().createKernel(IntegrateVVStepKernel::Name(), contextRef);
    vvKernel.getAs<IntegrateVVStepKernel>().initialize(contextRef.getSystem(), *this, force);
    if (!particlesNH.empty()) {
        nhKernel = context->getPlatform().createKernel(ModifyDrudeNoseKernel::Name(), contextRef);
        nhKernel.getAs<ModifyDrudeNoseKernel>().initialize(contextRef.getSystem(), *this, force);
    }
    if (!particlesLD.empty()) {
        ldKernel = context->getPlatform().createKernel(ModifyDrudeLangevinKernel::Name(), contextRef);
        ldKernel.getAs<ModifyDrudeLangevinKernel>().initialize(contextRef.getSystem(), *this, force, vvKernel);
    }
    if (!particlesImage.empty()) {
        imgKernel = context->getPlatform().createKernel(ModifyImageChargeKernel::Name(), contextRef);
        imgKernel.getAs<ModifyImageChargeKernel>().initialize(contextRef.getSystem(), *this);
    }
    if (!particlesElectrolyte.empty()){
        efKernel = context->getPlatform().createKernel(ModifyElectricFieldKernel::Name(), contextRef);
        efKernel.getAs<ModifyElectricFieldKernel>().initialize(contextRef.getSystem(), *this, vvKernel);
    }
    if (cosAcceleration!=0){
        ppKernel = context->getPlatform().createKernel(ModifyPeriodicPerturbationKernel::Name(), contextRef);
        ppKernel.getAs<ModifyPeriodicPerturbationKernel>().initialize(contextRef.getSystem(), *this, vvKernel);
    }
}

void VVIntegrator::cleanup() {
    vvKernel = Kernel();
    nhKernel = Kernel();
    ldKernel = Kernel();
    imgKernel = Kernel();
    efKernel = Kernel();
    ppKernel = Kernel();
}

vector<string> VVIntegrator::getKernelNames() {
    std::vector<std::string> names;
    names.push_back(IntegrateVVStepKernel::Name());
    names.push_back(ModifyDrudeNoseKernel::Name());
    names.push_back(ModifyDrudeLangevinKernel::Name());
    names.push_back(ModifyImageChargeKernel::Name());
    names.push_back(ModifyElectricFieldKernel::Name());
    names.push_back(ModifyPeriodicPerturbationKernel::Name());
    return names;
}

double VVIntegrator::computeKineticEnergy() {
    /**
     * Whenever the energies are queried, the force will possibly be reset to zero
     * So I must mark the force as invalid
     */
    forcesAreValid = false;
    return vvKernel.getAs<IntegrateVVStepKernel>().computeKineticEnergy(*context, *this);
}

void VVIntegrator::step(int steps) {
    if (context == NULL)
        throw OpenMMException("This Integrator is not bound to a context!");    
    for (int i = 0; i < steps; ++i) {

        /** TODO gongzheng @ 2020-02-21
         * The friction and random forces from Langevin thermostat
         * are calculated and stored separately from the normal FF Forces
         * So when the FF Forces are invalidated
         * (e.g. when barostat update the system or someone query the force and/or energy)
         * the stored Langevin forces are not affected
         *
         * After the first half velocity-verlet update
         * the FF forces are calculated from full-step position
         * and Langevin forces are calculated from half-step velocity
         *
         * Probably it's cleaner to implement Langevin thermostat as a Force object
         * then the Langevin force are calculated from full-step velocity
         */
        if (context->updateContextState())
            forcesAreValid = false;

        if (!forcesAreValid) {
            context->calcForcesAndEnergy(true, false);
            forcesAreValid = true;
        }

        // First half velocity verlet integrate (half-step velocity and full-step position update)
        if (!particlesNH.empty()){
            if (cosAcceleration != 0){
                ppKernel.getAs<ModifyPeriodicPerturbationKernel>().calcVelocityBias(*context, *this);
                ppKernel.getAs<ModifyPeriodicPerturbationKernel>().removeVelocityBias(*context, *this);
            }
            nhKernel.getAs<ModifyDrudeNoseKernel>().calcGroupKineticEnergies(*context, *this);
            nhKernel.getAs<ModifyDrudeNoseKernel>().scaleVelocity(*context, *this);
            if (cosAcceleration != 0){
                ppKernel.getAs<ModifyPeriodicPerturbationKernel>().restoreVelocityBias(*context, *this);
            }
        }
        vvKernel.getAs<IntegrateVVStepKernel>().firstIntegrate(*context, *this, forcesAreValid);

        // update the position of image particles
        if (!particlesImage.empty()){
            imgKernel.getAs<ModifyImageChargeKernel>().updateImagePositions(*context, *this);
        }

        // Calculate FF forces from full-step position
        context->calcForcesAndEnergy(true, false);
        forcesAreValid = true;
        // Calculate Langevin forces from half-step velocity and external electric force from charge
        if (!particlesLD.empty() || !particlesElectrolyte.empty() || cosAcceleration !=0)
            vvKernel.getAs<IntegrateVVStepKernel>().resetExtraForce(*context, *this);
        if (!particlesLD.empty())
            ldKernel.getAs<ModifyDrudeLangevinKernel>().applyLangevinForce(*context, *this);
        if (!particlesElectrolyte.empty())
            efKernel.getAs<ModifyElectricFieldKernel>().applyElectricForce(*context, *this);
        if (cosAcceleration != 0)
            ppKernel.getAs<ModifyPeriodicPerturbationKernel>().applyCosForce(*context, *this);

        // Second half velocity verlet integrate (full-step velocity update)
        vvKernel.getAs<IntegrateVVStepKernel>().secondIntegrate(*context, *this, forcesAreValid);
        if (!particlesNH.empty()) {
            if (cosAcceleration != 0){
                ppKernel.getAs<ModifyPeriodicPerturbationKernel>().calcVelocityBias(*context, *this);
                ppKernel.getAs<ModifyPeriodicPerturbationKernel>().removeVelocityBias(*context, *this);
            }
            nhKernel.getAs<ModifyDrudeNoseKernel>().calcGroupKineticEnergies(*context, *this);
            nhKernel.getAs<ModifyDrudeNoseKernel>().scaleVelocity(*context, *this);
            if (cosAcceleration != 0){
                ppKernel.getAs<ModifyPeriodicPerturbationKernel>().restoreVelocityBias(*context, *this);
            }
        }
    }
}

void VVIntegrator::propagateNHChain(std::vector<double> &eta, std::vector<double> &eta_dot,
                                    std::vector<double> &eta_dotdot, std::vector<double> &eta_mass,
                                    double ke2, double ke2_target, double t_target,
                                    double &factor) const {
    double expfac;
    double dt2 = getStepSize() / loopsPerStep / 2;
    double dt4 = dt2 / 2;
    double dt8 = dt4 / 2;

    factor = 1.0;
    if (eta_mass[0] > 0)
        eta_dotdot[0] = (ke2 - ke2_target) / eta_mass[0];
    for (int iloop = 0; iloop < loopsPerStep; iloop++) {
        for (int ich = numNHChains - 1; ich >= 0; ich--) {
            expfac = exp(-dt8 * eta_dot[ich + 1]);
            eta_dot[ich] *= expfac;
            eta_dot[ich] += eta_dotdot[ich] * dt4;
            eta_dot[ich] *= expfac;
        }
        factor *= exp(-dt2 * eta_dot[0]);

        for (int ich = 0; ich < numNHChains; ich++)
            eta[ich] += dt2 * eta_dot[ich];

        eta_dotdot[0] = (ke2 * factor * factor - ke2_target) / eta_mass[0];
        eta_dot[0] *= expfac;
        eta_dot[0] += eta_dotdot[0] * dt4;
        eta_dot[0] *= expfac;
        for (int ich = 1; ich < numNHChains; ich++) {
            expfac = exp(-dt8 * eta_dot[ich + 1]);
            eta_dot[ich] *= expfac;
            eta_dotdot[ich] = (eta_mass[ich - 1] * eta_dot[ich - 1] * eta_dot[ich - 1]
                               - BOLTZ * t_target) / eta_mass[ich];
            eta_dot[ich] += eta_dotdot[ich] * dt4;
            eta_dot[ich] *= expfac;
        }
    }
}

std::vector<double> VVIntegrator::getViscosity() {
    double vMax = 0, invVis = 0;
    if (cosAcceleration != 0)
        ppKernel.getAs<ModifyPeriodicPerturbationKernel>().calcViscosity(*context, *this, vMax, invVis);
    return std::vector<double>{vMax, invVis};
}