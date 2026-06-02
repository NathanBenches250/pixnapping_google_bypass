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

#define ATRACE_TAG ATRACE_TAG_GRAPHICS
#include "BlurFilter.h"
#include <SkBlendMode.h>
#include <SkCanvas.h>
#include <SkPaint.h>
#include <SkRRect.h>
#include <SkRuntimeEffect.h>
#include <SkSize.h>
#include <SkString.h>
#include <SkSurface.h>
#include <SkTileMode.h>
#include <common/trace.h>
#include <log/log.h>

namespace android {
namespace renderengine {
namespace skia {

static sk_sp<SkRuntimeEffect> createMixEffect() {
    SkString mixString(R"(
        uniform shader blurredInput;
        uniform shader originalInput;
        uniform float mixFactor;

        half4 main(float2 xy) {
            return half4(mix(originalInput.eval(xy), blurredInput.eval(xy), mixFactor)).rgb1;
        }
    )");

    auto [mixEffect, mixError] = SkRuntimeEffect::MakeForShader(mixString);
    if (!mixEffect) {
        LOG_ALWAYS_FATAL("RuntimeShader error: %s", mixError.c_str());
    }
    return mixEffect;
}

static SkMatrix getShaderTransform(const SkCanvas* canvas, const SkRect& blurRect,
                                   const float scale) {
    // 1. Apply the blur shader matrix, which scales up the blurred surface to its real size
    auto matrix = SkMatrix::Scale(scale, scale); // since scale down to save GPU memory, we scale back up our blurred image
    // 2. Since the blurred surface has the size of the layer, we align it with the
    // top left corner of the layer position.
    matrix.postConcat(SkMatrix::Translate(blurRect.fLeft, blurRect.fTop)); // align the image at the right coordinates of the layer
    // 3. Finally, apply the inverse canvas matrix. The snapshot made in the BlurFilter is in the
    // original surface orientation. The inverse matrix has to be applied to align the blur
    // surface with the current orientation/position of the canvas.
    SkMatrix drawInverse;
    if (canvas != nullptr && canvas->getTotalMatrix().invert(&drawInverse)) { // if the phone was flipped etc. the matrix would also flip our blurred image so to prevent that we inverse it.
        matrix.postConcat(drawInverse);
    }
    return matrix;
}

BlurFilter::BlurFilter(const float maxCrossFadeRadius)
      : mMaxCrossFadeRadius(maxCrossFadeRadius),
        mMixEffect(maxCrossFadeRadius > 0 ? createMixEffect() : nullptr) {}

float BlurFilter::getMaxCrossFadeRadius() const {
    return mMaxCrossFadeRadius;
}

void BlurFilter::drawBlurRegion(SkCanvas* canvas, const SkRRect& effectRegion, // where to draw to, where in canvas to draw
                                const uint32_t blurRadius, const float blurAlpha,
                                const SkRect& blurRect, sk_sp<SkImage> blurredImage, // blur bounds in snapshot, output to generate
                                sk_sp<SkImage> input) { 
    SFTRACE_CALL();

    SkPaint paint; 
    paint.setAlphaf(blurAlpha);

    const auto blurMatrix = getShaderTransform(canvas, blurRect, kInverseInputScale);  // It produces blurMatrix, which holds the exact translation instructions needed to counteract whatever scaling or rotations your app window is doing, keeping the background blur perfectly anchored to the physical screen coordinates.
    SkSamplingOptions linearSampling(SkFilterMode::kLinear, SkMipmapMode::kNone);  // set up bilinear filtering
    const auto blurShader = blurredImage->makeShader(SkTileMode::kClamp, SkTileMode::kClamp,
                                                     linearSampling, &blurMatrix);  // create a shader that will apply the blurMatrix to the blurredImage

    if (blurRadius < mMaxCrossFadeRadius) { // since blurs at < 10px looks messy, 
        // For sampling Skia's API expects the inverse of what logically seems appropriate. In this
        // case you might expect the matrix to simply be the canvas matrix.
        SkMatrix inputMatrix;
        if (!canvas->getTotalMatrix().invert(&inputMatrix)) {
            ALOGE("matrix was unable to be inverted");
        }

        SkRuntimeShaderBuilder blurBuilder(mMixEffect);
        blurBuilder.child("blurredInput") = blurShader;
        blurBuilder.child("originalInput") =
                input->makeShader(SkTileMode::kClamp, SkTileMode::kClamp, linearSampling,
                                  inputMatrix);
        blurBuilder.uniform("mixFactor") = blurRadius / mMaxCrossFadeRadius;

        paint.setShader(blurBuilder.makeShader());
    } else {
        paint.setShader(blurShader);
    }

    if (effectRegion.isRect()) {
        if (blurAlpha == 1.0f) {
            paint.setBlendMode(SkBlendMode::kSrc);
        }
        canvas->drawRect(effectRegion.rect(), paint);
    } else {
        paint.setAntiAlias(true);
        canvas->drawRRect(effectRegion, paint);
    }
}

} // namespace skia
} // namespace renderengine
} // namespace android
