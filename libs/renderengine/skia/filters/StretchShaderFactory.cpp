/*
 * Copyright 2021 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "StretchShaderFactory.h"
#include <SkImageFilter.h>
#include <SkRefCnt.h>
#include <SkRuntimeEffect.h>
#include <SkString.h>
#include <SkSurface.h>
#include "log/log.h"
#include <memory>

namespace android {
namespace renderengine {
namespace skia {

static const SkString stretchShader = SkString(R"(
    uniform shader uContentTexture;

    // multiplier to apply to scale effect
    uniform float uMaxStretchIntensity;

    // Maximum percentage to stretch beyond bounds  of target
    uniform float uStretchAffectedDistX;
    uniform float uStretchAffectedDistY;

    // Distance stretched as a function of the normalized overscroll times
    // scale intensity
    uniform float uDistanceStretchedX;
    uniform float uDistanceStretchedY;
    uniform float uInverseDistanceStretchedX;
    uniform float uInverseDistanceStretchedY;
    uniform float uDistDiffX;

    // Difference between the peak stretch amount and overscroll amount normalized
    uniform float uDistDiffY;

    // Horizontal offset represented as a ratio of pixels divided by the target width
    uniform float uScrollX;
    // Vertical offset represented as a ratio of pixels divided by the target height
    uniform float uScrollY;

    // Normalized overscroll amount in the horizontal direction
    uniform float uOverscrollX;

    // Normalized overscroll amount in the vertical direction
    uniform float uOverscrollY;
    uniform float viewportWidth; // target height in pixels
    uniform float viewportHeight; // target width in pixels

    // uInterpolationStrength is the intensity of the interpolation.
    // if uInterpolationStrength is 0, then the stretch is constant for all the
    // uStretchAffectedDist. if uInterpolationStrength is 1, then stretch intensity
    // is interpolated based on the pixel position in the uStretchAffectedDist area;
    // The closer we are from the scroll anchor point, the more it stretches,
    // and the other way around.
    uniform float uInterpolationStrength;

    float easeIn(float t, float d) {
        return t * d;
    }

    float computeOverscrollStart(
        float inPos,
        float overscroll,
        float uStretchAffectedDist,
        float uInverseStretchAffectedDist,
        float distanceStretched,
        float interpolationStrength
    ) {
        float offsetPos = uStretchAffectedDist - inPos;
        float posBasedVariation = mix(
                1. ,easeIn(offsetPos, uInverseStretchAffectedDist), interpolationStrength);
        float stretchIntensity = overscroll * posBasedVariation;
        return distanceStretched - (offsetPos / (1. + stretchIntensity));
    }

    float computeOverscrollEnd(
        float inPos,
        float overscroll,
        float reverseStretchDist,
        float uStretchAffectedDist,
        float uInverseStretchAffectedDist,
        float distanceStretched,
        float interpolationStrength
    ) {
        float offsetPos = inPos - reverseStretchDist;
        float posBasedVariation = mix(
                1. ,easeIn(offsetPos, uInverseStretchAffectedDist), interpolationStrength);
        float stretchIntensity = (-overscroll) * posBasedVariation;
        return 1 - (distanceStretched - (offsetPos / (1. + stretchIntensity)));
    }

    // Prefer usage of return values over out parameters as it enables
    // SKSL to properly inline method calls and works around potential GPU
    // driver issues on Wembly. See b/182566543 for details
    float computeOverscroll(
        float inPos,
        float overscroll,
        float uStretchAffectedDist,
        float uInverseStretchAffectedDist,
        float distanceStretched,
        float distanceDiff,
        float interpolationStrength
    ) {
      float outPos = inPos;
      // overscroll is provided via uniform so there is no concern
      // for potential incoherent branches
      if (overscroll > 0) {
            if (inPos <= uStretchAffectedDist) {
                outPos = computeOverscrollStart(
                  inPos,
                  overscroll,
                  uStretchAffectedDist,
                  uInverseStretchAffectedDist,
                  distanceStretched,
                  interpolationStrength
                );
            } else if (inPos >= distanceStretched) {
                outPos = distanceDiff + inPos;
            }
        }
        if (overscroll < 0) {
            float stretchAffectedDist = 1. - uStretchAffectedDist;
            if (inPos >= stretchAffectedDist) {
                outPos = computeOverscrollEnd(
                  inPos,
                  overscroll,
                  stretchAffectedDist,
                  uStretchAffectedDist,
                  uInverseStretchAffectedDist,
                  distanceStretched,
                  interpolationStrength
                );
            } else if (inPos < stretchAffectedDist) {
                outPos = -distanceDiff + inPos;
            }
        }
        return outPos;
    }

    vec4 main(vec2 coord) {
        // Normalize SKSL pixel coordinate into a unit vector
        float inU = coord.x / viewportWidth;
        float inV = coord.y / viewportHeight;
        float outU;
        float outV;
        float stretchIntensity;
        // Add the normalized scroll position within scrolling list
        inU += uScrollX;
        inV += uScrollY;
        outU = inU;
        outV = inV;
        outU = computeOverscroll(
            inU,
            uOverscrollX,
            uStretchAffectedDistX,
            uInverseDistanceStretchedX,
            uDistanceStretchedX,
            uDistDiffX,
            uInterpolationStrength
        );
        outV = computeOverscroll(
            inV,
            uOverscrollY,
            uStretchAffectedDistY,
            uInverseDistanceStretchedY,
            uDistanceStretchedY,
            uDistDiffY,
            uInterpolationStrength
        );
        coord.x = (outU - uScrollX) * viewportWidth;
        coord.y = (outV - uScrollY) * viewportHeight;
        return uContentTexture.eval(coord);
    })");

const float INTERPOLATION_STRENGTH_VALUE = 0.7f;

sk_sp<SkShader> StretchShaderFactory::createSkShader(const sk_sp<SkShader>& inputShader,
                                                     const StretchEffect& stretchEffect) {
    if (!stretchEffect.hasEffect()) {
        return nullptr;
    }

    float viewportWidth = stretchEffect.width;
    float viewportHeight = stretchEffect.height;
    float normOverScrollDistX = stretchEffect.vectorX;
    float normOverScrollDistY = stretchEffect.vectorY;
    float distanceStretchedX =
        StretchEffect::CONTENT_DISTANCE_STRETCHED / (1 + abs(normOverScrollDistX));
    float distanceStretchedY =
        StretchEffect::CONTENT_DISTANCE_STRETCHED / (1 + abs(normOverScrollDistY));
    float inverseDistanceStretchedX =
        1.f / StretchEffect::CONTENT_DISTANCE_STRETCHED;
    float inverseDistanceStretchedY =
        1.f / StretchEffect::CONTENT_DISTANCE_STRETCHED;
    float diffX =
        distanceStretchedX - StretchEffect::CONTENT_DISTANCE_STRETCHED;
    float diffY =
        distanceStretchedY - StretchEffect::CONTENT_DISTANCE_STRETCHED;
    auto& srcBounds = stretchEffect.mappedChildBounds;
    float normalizedScrollX = srcBounds.left / viewportWidth;
    float normalizedScrollY = srcBounds.top / viewportHeight;

    if (mBuilder == nullptr) {
        const static SkRuntimeEffect::Result instance =
            SkRuntimeEffect::MakeForShader(stretchShader);
        mBuilder = std::make_unique<SkRuntimeShaderBuilder>(instance.effect);
    }

    mBuilder->child("uContentTexture") = inputShader;
    mBuilder->uniform("uInterpolationStrength").set(&INTERPOLATION_STRENGTH_VALUE, 1);
    mBuilder->uniform("uStretchAffectedDistX").set(&StretchEffect::CONTENT_DISTANCE_STRETCHED, 1);
    mBuilder->uniform("uStretchAffectedDistY").set(&StretchEffect::CONTENT_DISTANCE_STRETCHED, 1);
    mBuilder->uniform("uDistanceStretchedX").set(&distanceStretchedX, 1);
    mBuilder->uniform("uDistanceStretchedY").set(&distanceStretchedY, 1);
    mBuilder->uniform("uInverseDistanceStretchedX").set(&inverseDistanceStretchedX, 1);
    mBuilder->uniform("uInverseDistanceStretchedY").set(&inverseDistanceStretchedY, 1);
    mBuilder->uniform("uDistDiffX").set(&diffX, 1);
    mBuilder->uniform("uDistDiffY").set(&diffY, 1);
    mBuilder->uniform("uOverscrollX").set(&normOverScrollDistX, 1);
    mBuilder->uniform("uOverscrollY").set(&normOverScrollDistY, 1);
    mBuilder->uniform("uScrollX").set(&normalizedScrollX, 1);
    mBuilder->uniform("uScrollY").set(&normalizedScrollY, 1);
    mBuilder->uniform("viewportWidth").set(&viewportWidth, 1);
    mBuilder->uniform("viewportHeight").set(&viewportHeight, 1);

    return mBuilder->makeShader();
}

} // namespace skia
} // namespace renderengine
} // namespace android