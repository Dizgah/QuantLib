/* -*- mode: c++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

/*
 Copyright (C) 2005, 2007 Klaus Spanderen

 This file is part of QuantLib, a free-software/open-source library
 for financial quantitative analysts and developers - http://quantlib.org/

 QuantLib is free software: you can redistribute it and/or modify it
 under the terms of the QuantLib license.  You should have received a
 copy of the license along with this program; if not, please email
 <quantlib-dev@lists.sf.net>. The license is also available online at
 <http://quantlib.org/license.shtml>.

 This program is distributed in the hope that it will be useful, but WITHOUT
 ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 FOR A PARTICULAR PURPOSE.  See the license for more details.
*/

#include <ql/math/distributions/normaldistribution.hpp>
#include <ql/math/distributions/chisquaredistribution.hpp>
#include <ql/quotes/simplequote.hpp>
#include <ql/processes/hestonprocess.hpp>
#include <ql/processes/eulerdiscretization.hpp>

namespace QuantLib {

    HestonProcess::HestonProcess(
                              const Handle<YieldTermStructure>& riskFreeRate,
                              const Handle<YieldTermStructure>& dividendYield,
                              const Handle<Quote>& s0,
                              double v0, double kappa,
                              double theta, double sigma, double rho,
                              Discretization d)
    : StochasticProcess(boost::shared_ptr<discretization>(
                                                    new EulerDiscretization)),
      riskFreeRate_(riskFreeRate), dividendYield_(dividendYield), s0_(s0),
      v0_(v0), kappa_(kappa), theta_(theta), sigma_(sigma), rho_(rho),
      discretization_(d) {

        registerWith(riskFreeRate_);
        registerWith(dividendYield_);
        registerWith(s0_);
    }

    Size HestonProcess::size() const {
        return 2;
    }

    Disposable<Array> HestonProcess::initialValues() const {
        Array tmp(2);
        tmp[0] = s0_->value();
        tmp[1] = v0_;
        return tmp;
    }

    Disposable<Array> HestonProcess::drift(Time t, const Array& x) const {
        Array tmp(2);
        const Real vol = (x[1] > 0.0) ? std::sqrt(x[1])
                         : (discretization_ == Reflection) ? - std::sqrt(-x[1])
                         : 0.0;

        tmp[0] = riskFreeRate_->forwardRate(t, t, Continuous)
               - dividendYield_->forwardRate(t, t, Continuous)
               - 0.5 * vol * vol;

        tmp[1] = kappa_*
           (theta_-((discretization_==PartialTruncation) ? x[1] : vol*vol));
        return tmp;
    }

    Disposable<Matrix> HestonProcess::diffusion(Time, const Array& x) const {
        /* the correlation matrix is
           |  1   rho |
           | rho   1  |
           whose square root (which is used here) is
           |  1          0       |
           | rho   sqrt(1-rho^2) |
        */
        Matrix tmp(2,2);
        const Real vol = (x[1] > 0.0) ? std::sqrt(x[1])
                         : (discretization_ == Reflection) ? -std::sqrt(-x[1])
                         : 1e-8; // set vol to (almost) zero but still
                                 // expose some correlation information
        const Real sigma2 = sigma_ * vol;
        const Real sqrhov = std::sqrt(1.0 - rho_*rho_);

        tmp[0][0] = vol;          tmp[0][1] = 0.0;
        tmp[1][0] = rho_*sigma2;  tmp[1][1] = sqrhov*sigma2;
        return tmp;
    }

    Disposable<Array> HestonProcess::apply(const Array& x0,
                                           const Array& dx) const {
        Array tmp(2);
        tmp[0] = x0[0] * std::exp(dx[0]);
        tmp[1] = x0[1] + dx[1];
        return tmp;
    }

    Disposable<Array> HestonProcess::evolve(Time t0, const Array& x0,
                                            Time dt, const Array& dw) const {
        Array retVal(2);
        Real ncp, df, p, dy;
        Real vol, vol2, mu, nu;

        const Real sdt = std::sqrt(dt);
        const Real sqrhov = std::sqrt(1.0 - rho_*rho_);

        switch (discretization_) {
          // For the definition of PartialTruncation, FullTruncation
          // and Reflection  see Lord, R., R. Koekkoek and D. van Dijk (2006),
          // "A Comparison of biased simulation schemes for
          //  stochastic volatility models",
          // Working Paper, Tinbergen Institute
          case PartialTruncation:
            vol = (x0[1] > 0.0) ? std::sqrt(x0[1]) : 0.0;
            vol2 = sigma_ * vol;
            mu =    riskFreeRate_->forwardRate(t0, t0+dt, Continuous)
                  - dividendYield_->forwardRate(t0, t0+dt, Continuous)
                    - 0.5 * vol * vol;
            nu = kappa_*(theta_ - x0[1]);

            retVal[0] = x0[0] * std::exp(mu*dt+vol*dw[0]*sdt);
            retVal[1] = x0[1] + nu*dt + vol2*sdt*(rho_*dw[0] + sqrhov*dw[1]);
            break;
          case FullTruncation:
            vol = (x0[1] > 0.0) ? std::sqrt(x0[1]) : 0.0;
            vol2 = sigma_ * vol;
            mu =    riskFreeRate_->forwardRate(t0, t0+dt, Continuous)
                  - dividendYield_->forwardRate(t0, t0+dt, Continuous)
                    - 0.5 * vol * vol;
            nu = kappa_*(theta_ - vol*vol);

            retVal[0] = x0[0] * std::exp(mu*dt+vol*dw[0]*sdt);
            retVal[1] = x0[1] + nu*dt + vol2*sdt*(rho_*dw[0] + sqrhov*dw[1]);
            break;
          case Reflection:
            vol = std::sqrt(std::fabs(x0[1]));
            vol2 = sigma_ * vol;
            mu =    riskFreeRate_->forwardRate(t0, t0+dt, Continuous)
                  - dividendYield_->forwardRate(t0, t0+dt, Continuous)
                    - 0.5 * vol*vol;
            nu = kappa_*(theta_ - vol*vol);

            retVal[0] = x0[0]*std::exp(mu*dt+vol*dw[0]*sdt);
            retVal[1] = vol*vol
                        +nu*dt + vol2*sdt*(rho_*dw[0] + sqrhov*dw[1]);
            break;
          case ExactVariance:
            // use Alan Lewis trick to decorrelate the equity and the variance
            // process by using y(t)=x(t)-\frac{rho}{sigma}\nu(t)
            // and Ito's Lemma. Then use exact sampling for the variance
            // process. For further details please read the wilmott thread
            // "QuantLib code is very high quality"
            vol = (x0[1] > 0.0) ? std::sqrt(x0[1]) : 0.0;
            mu =   riskFreeRate_->forwardRate(t0, t0+dt, Continuous)
                 - dividendYield_->forwardRate(t0, t0+dt, Continuous)
                   - 0.5 * vol*vol;

            df  = 4*theta_*kappa_/(sigma_*sigma_);
            ncp = 4*kappa_*std::exp(-kappa_*dt)
                /(sigma_*sigma_*(1-std::exp(-kappa_*dt)))*x0[1];

            p = CumulativeNormalDistribution()(dw[1]);
            if (p<0.0)
                p = 0.0;
            else if (p >= 1.0)
                p = 1.0-QL_EPSILON;

            retVal[1] = sigma_*sigma_*(1-std::exp(-kappa_*dt))/(4*kappa_)
                *InverseNonCentralChiSquareDistribution(df, ncp, 100)(p);

            dy = (mu - rho_/sigma_*kappa_
                          *(theta_-vol*vol)) * dt + vol*sqrhov*dw[0]*sdt;

            retVal[0] = x0[0]*std::exp(dy + rho_/sigma_*(retVal[1]-x0[1]));
            break;
          default:
            QL_FAIL("unknown discretization schema");
        }

        return retVal;
    }

    const Handle<Quote>& HestonProcess::s0() const {
        return s0_;
    }

    const Handle<YieldTermStructure>& HestonProcess::dividendYield() const {
        return dividendYield_;
    }

    const Handle<YieldTermStructure>& HestonProcess::riskFreeRate() const {
        return riskFreeRate_;
    }

    Time HestonProcess::time(const Date& d) const {
        return riskFreeRate_->dayCounter().yearFraction(
                                           riskFreeRate_->referenceDate(), d);
    }

}
