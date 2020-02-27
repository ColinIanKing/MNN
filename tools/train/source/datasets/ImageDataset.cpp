//
//  ImageDataset.cpp
//  MNN
//
//  Created by MNN on 2019/12/30.
//  Copyright © 2018, Alibaba Group Holding Limited
//

#include "ImageDataset.hpp"
#include <stdlib.h>
#include <string.h>
#include <fstream>
#include <string>
#include <vector>
//#define STB_IMAGE_IMPLEMENTATION
#include "MNN/ImageProcess.hpp"
#include "MNN/MNNDefine.h"
#include "stb_image.h"
#include "RandomGenerator.hpp"

using namespace std;
using namespace MNN::CV;

// behave like python split
vector<string> split(const string sourceStr, string splitChar = " ") {
    vector<string> result;
    int pos   = 0;
    int start = 0;

    while ((pos = sourceStr.find(splitChar, start)) != string::npos) {
        result.emplace_back(sourceStr.substr(start, pos - start));
        start = pos + splitChar.size();
    }

    if (start < sourceStr.size()) {
        result.emplace_back(sourceStr.substr(start));
    }

    return result;
}

ImageDataset::ImageDataset(const std::string pathToImages, const std::string pathToImageTxt, ImageConfig cfg,
                           bool readAllToMemory) {
    mReadAllToMemory = readAllToMemory;
    mConfig          = cfg;

    mProcessConfig.sourceFormat = ImageFormat::RGBA;
    mProcessConfig.filterType   = MNN::CV::BILINEAR;

    for (int i = 0; i < cfg.mean.size(); i++) {
        mProcessConfig.normal[i] = cfg.scale[i];
        mProcessConfig.mean[i] = cfg.mean[i];
    }

    switch (cfg.destFormat) {
        case DestImageFormat::GRAY:
            mProcessConfig.destFormat = ImageFormat::GRAY;
            break;
        case DestImageFormat::RGB:
            mProcessConfig.destFormat = ImageFormat::RGB;
            break;
        case DestImageFormat::BGR:
            mProcessConfig.destFormat = ImageFormat::BGR;
            break;
        default:
            MNN_PRINT("not supported dest format\n");
            MNN_ASSERT(false);
            break;
    }

    getAllDataAndLabelsFromTxt(pathToImages, pathToImageTxt);

    if (mReadAllToMemory) {
        for (int i = 0; i < mAllTxtLines.size(); i++) {
            auto dataLabelsPair = getDataAndLabelsFrom(mAllTxtLines[i]);
            mDataAndLabels.emplace_back(dataLabelsPair);
        }
    }
}

Example ImageDataset::get(size_t index) {
    if (mReadAllToMemory) {
        return {{mDataAndLabels[index].first}, {mDataAndLabels[index].second}};
    } else {
        auto dataAndLabels = getDataAndLabelsFrom(mAllTxtLines[index]);
        return {{dataAndLabels.first}, {dataAndLabels.second}};
    }
}

size_t ImageDataset::size() {
    return mAllTxtLines.size();
}

void ImageDataset::getAllDataAndLabelsFromTxt(const std::string pathToImages, std::string pathToImageTxt) {
    std::ifstream txtFile(pathToImageTxt);
    if (!txtFile.is_open()) {
        MNN_PRINT("%s: file not found\n", pathToImageTxt.c_str());
        MNN_ASSERT(false);
    }
    string line;
    while (getline(txtFile, line)) {
        vector<string> splitStr;
        splitStr = split(line, " ");
        if (splitStr.size() != 2) {
            MNN_PRINT("%s: file format error\n", pathToImageTxt.c_str());
            MNN_ASSERT(false);
        }
        std::pair<std::string, std::vector<int> > dataPair;
        dataPair.first = pathToImages + splitStr[0];
        vector<string> labels;
        labels = split(splitStr[1], ",");
        for (int i = 0; i < labels.size(); i++) {
            dataPair.second.emplace_back(atoi(labels[i].c_str()));
        }
        mAllTxtLines.emplace_back(dataPair);
    }
    txtFile.close();
}

std::pair<VARP, VARP> ImageDataset::getDataAndLabelsFrom(std::pair<std::string, std::vector<int> > dataAndLabels) {
    int originalWidth, originalHeight, comp;
    string imageName  = dataAndLabels.first;
    auto bitmap32bits = stbi_load(imageName.c_str(), &originalWidth, &originalHeight, &comp, 4);
    if (bitmap32bits == nullptr) {
        MNN_PRINT("can not open image: %s\n", imageName.c_str());
        MNN_ASSERT(false);
    }

    // choose resize or crop
    // resize method
    int oh, ow, bpp;
    if (mConfig.resizeHeight > 0 && mConfig.resizeWidth > 0) {
        oh = mConfig.resizeHeight;
        ow = mConfig.resizeWidth;
    } else {
        oh = originalHeight;
        ow = originalWidth;
    }
    bpp = mConfig.destFormat == DestImageFormat::GRAY ? 1 : 3;

    std::shared_ptr<MNN::CV::ImageProcess> process;
    process.reset(ImageProcess::create(mProcessConfig));

    if (abs(mConfig.cropFraction[0] - 1.) > 1e-6 || abs(mConfig.cropFraction[1] - 1.) > 1e-6) {
        const float cropFractionH = mConfig.cropFraction[0];
        const float cropFractionW = mConfig.cropFraction[1];

        const int hCropSize = int(originalHeight * cropFractionH);
        const int wCropSize = int(originalWidth * cropFractionW);
        MNN_ASSERT(hCropSize > 0 && wCropSize > 0);
        // default center crop
        int startH = (originalHeight - hCropSize) / 2;
        int startW = (originalWidth - wCropSize) / 2;

        if (mConfig.centerOrRandomCrop == true) {
            const int maxStartPointH = originalHeight - hCropSize;
            const int maxStartPointW = originalWidth - wCropSize;
            // generate a random number between (0, maxPoint)
            auto gen = RandomGenerator::generator();
            std::uniform_int_distribution<> disH(0, maxStartPointH);
            startH = disH(gen);
            std::uniform_int_distribution<> disW(0, maxStartPointW);
            startW = disW(gen);
        }

        const int endH = startH + hCropSize;
        const int endW = startW + wCropSize;

        float srcPoints[] = {
                float(startW), float(startH),
                float(startW), float(endH - 1),
                float(endW - 1), float(startH),
                float(endW - 1), float(endH - 1),
        };
        float dstPoints[] = {
                0.0f, 0.0f,
                0.0f, float(oh - 1),
                float(ow - 1), 0.0f,
                float(ow - 1), float(oh - 1),
        };
        MNN::CV::Matrix trans;
        trans.setPolyToPoly((MNN::CV::Point*)dstPoints, (MNN::CV::Point*)srcPoints, 4);
        process->setMatrix(trans);
    } else {
        if (mConfig.resizeHeight > 0 && mConfig.resizeWidth > 0) {
            float srcPoints[] = {
                    float(0), float(0),
                    float(0), float(originalHeight - 1),
                    float(originalWidth - 1), float(0),
                    float(originalWidth - 1), float(originalHeight - 1),
            };
            float dstPoints[] = {
                    0.0f, 0.0f,
                    0.0f, float(oh - 1),
                    float(ow - 1), 0.0f,
                    float(ow - 1), float(oh - 1),
            };
            MNN::CV::Matrix trans;
            trans.setPolyToPoly((MNN::CV::Point*)dstPoints, (MNN::CV::Point*)srcPoints, 4);
            process->setMatrix(trans);
        }
    }

    auto data      = _Input({oh, ow, bpp}, NHWC, halide_type_of<float>());
    auto txtLabels = dataAndLabels.second;
    auto labels    = _Input({int(txtLabels.size())}, NHWC, halide_type_of<int32_t>());

    process->convert(bitmap32bits, originalWidth, originalHeight, 0, data->writeMap<float>(), ow, oh, bpp, ow * bpp,
                      halide_type_of<float>());

    auto labelsDataPtr = labels->writeMap<int32_t>();
    for (int j = 0; j < txtLabels.size(); j++) {
        labelsDataPtr[j] = txtLabels[j];
    }
    stbi_image_free(bitmap32bits);

    return std::make_pair(data, labels);
}