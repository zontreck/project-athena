//
//  FBXBaker.cpp
//  tools/baking/src
//
//  Created by Stephen Birarda on 3/30/17.
//  Copyright 2017 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include <cmath> // need this include so we don't get an error looking for std::isnan

#include <QtConcurrent>
#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QEventLoop>
#include <QtCore/QFileInfo>
#include <QtCore/QThread>

#include <mutex>

#include <NetworkAccessManager.h>
#include <SharedUtil.h>

#include <PathUtils.h>

#include <FBXReader.h>
#include <FBXWriter.h>

#include "ModelBakingLoggingCategory.h"
#include "TextureBaker.h"

#include "FBXBaker.h"

FBXBaker::FBXBaker(const QUrl& fbxURL, TextureBakerThreadGetter textureThreadGetter,
                   const QString& bakedOutputDir, const QString& originalOutputDir) : 
    ModelBaker(fbxURL, textureThreadGetter, bakedOutputDir, originalOutputDir) 
{

}
   
FBXBaker::~FBXBaker() {
    if (_tempDir.exists()) {
        if (!_tempDir.remove(_originalFBXFilePath)) {
            qCWarning(model_baking) << "Failed to remove temporary copy of fbx file:" << _originalFBXFilePath;
        }
        if (!_tempDir.rmdir(".")) {
            qCWarning(model_baking) << "Failed to remove temporary directory:" << _tempDir;
        }
    }
}

void FBXBaker::abort() {
    Baker::abort();

    // tell our underlying TextureBaker instances to abort
    // the FBXBaker will wait until all are aborted before emitting its own abort signal
    for (auto& textureBaker : _bakingTextures) {
        textureBaker->abort();
    }
}

void FBXBaker::bake() {
    qDebug() << "FBXBaker" << _modelURL << "bake starting";

    auto tempDir = PathUtils::generateTemporaryDir();

    if (tempDir.isEmpty()) {
        handleError("Failed to create a temporary directory.");
        return;
    }

    _tempDir = tempDir;

    _originalFBXFilePath = _tempDir.filePath(_modelURL.fileName());
    qDebug() << "Made temporary dir " << _tempDir;
    qDebug() << "Origin file path: " << _originalFBXFilePath;

    // setup the output folder for the results of this bake
    setupOutputFolder();

    if (shouldStop()) {
        return;
    }

    connect(this, &FBXBaker::sourceCopyReadyToLoad, this, &FBXBaker::bakeSourceCopy);

    // make a local copy of the FBX file
    loadSourceFBX();
}

void FBXBaker::bakeSourceCopy() {
    // load the scene from the FBX file
    importScene();

    if (shouldStop()) {
        return;
    }

    // enumerate the models and textures found in the scene and start a bake for them
    rewriteAndBakeSceneTextures();

    if (shouldStop()) {
        return;
    }

    rewriteAndBakeSceneModels();

    if (shouldStop()) {
        return;
    }

    // export the FBX with re-written texture references
    exportScene();

    if (shouldStop()) {
        return;
    }

    // check if we're already done with textures (in case we had none to re-write)
    checkIfTexturesFinished();
}

void FBXBaker::setupOutputFolder() {
    // make sure there isn't already an output directory using the same name
    if (QDir(_bakedOutputDir).exists()) {
        qWarning() << "Output path" << _bakedOutputDir << "already exists. Continuing.";
    } else {
        qCDebug(model_baking) << "Creating FBX output folder" << _bakedOutputDir;

        // attempt to make the output folder
        if (!QDir().mkpath(_bakedOutputDir)) {
            handleError("Failed to create FBX output folder " + _bakedOutputDir);
            return;
        }
        // attempt to make the output folder
        if (!QDir().mkpath(_originalOutputDir)) {
            handleError("Failed to create FBX output folder " + _bakedOutputDir);
            return;
        }
    }
}

void FBXBaker::loadSourceFBX() {
    // check if the FBX is local or first needs to be downloaded
    if (_modelURL.isLocalFile()) {
        // load up the local file
        QFile localFBX { _modelURL.toLocalFile() };

        qDebug() << "Local file url: " << _modelURL << _modelURL.toString() << _modelURL.toLocalFile() << ", copying to: " << _originalFBXFilePath;

        if (!localFBX.exists()) {
            //QMessageBox::warning(this, "Could not find " + _fbxURL.toString(), "");
            handleError("Could not find " + _modelURL.toString());
            return;
        }

        // make a copy in the output folder
        if (!_originalOutputDir.isEmpty()) {
            qDebug() << "Copying to: " << _originalOutputDir << "/" << _modelURL.fileName();
            localFBX.copy(_originalOutputDir + "/" + _modelURL.fileName());
        }

        localFBX.copy(_originalFBXFilePath);

        // emit our signal to start the import of the FBX source copy
        emit sourceCopyReadyToLoad();
    } else {
        // remote file, kick off a download
        auto& networkAccessManager = NetworkAccessManager::getInstance();

        QNetworkRequest networkRequest;

        // setup the request to follow re-directs and always hit the network
        networkRequest.setAttribute(QNetworkRequest::FollowRedirectsAttribute, true);
        networkRequest.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
        networkRequest.setHeader(QNetworkRequest::UserAgentHeader, HIGH_FIDELITY_USER_AGENT);

        networkRequest.setUrl(_modelURL);

        qCDebug(model_baking) << "Downloading" << _modelURL;
        auto networkReply = networkAccessManager.get(networkRequest);

        connect(networkReply, &QNetworkReply::finished, this, &FBXBaker::handleFBXNetworkReply);
    }
}

void FBXBaker::handleFBXNetworkReply() {
    auto requestReply = qobject_cast<QNetworkReply*>(sender());

    if (requestReply->error() == QNetworkReply::NoError) {
        qCDebug(model_baking) << "Downloaded" << _modelURL;

        // grab the contents of the reply and make a copy in the output folder
        QFile copyOfOriginal(_originalFBXFilePath);

        qDebug(model_baking) << "Writing copy of original FBX to" << _originalFBXFilePath << copyOfOriginal.fileName();

        if (!copyOfOriginal.open(QIODevice::WriteOnly)) {
            // add an error to the error list for this FBX stating that a duplicate of the original FBX could not be made
            handleError("Could not create copy of " + _modelURL.toString() + " (Failed to open " + _originalFBXFilePath + ")");
            return;
        }
        if (copyOfOriginal.write(requestReply->readAll()) == -1) {
            handleError("Could not create copy of " + _modelURL.toString() + " (Failed to write)");
            return;
        }

        // close that file now that we are done writing to it
        copyOfOriginal.close();

        if (!_originalOutputDir.isEmpty()) {
            copyOfOriginal.copy(_originalOutputDir + "/" + _modelURL.fileName());
        }

        // emit our signal to start the import of the FBX source copy
        emit sourceCopyReadyToLoad();
    } else {
        // add an error to our list stating that the FBX could not be downloaded
        handleError("Failed to download " + _modelURL.toString());
    }
}

void FBXBaker::importScene() {
    qDebug() << "file path: " << _originalFBXFilePath.toLocal8Bit().data() << QDir(_originalFBXFilePath).exists();

    QFile fbxFile(_originalFBXFilePath);
    if (!fbxFile.open(QIODevice::ReadOnly)) {
        handleError("Error opening " + _originalFBXFilePath + " for reading");
        return;
    }

    FBXReader reader;

    qCDebug(model_baking) << "Parsing" << _modelURL;
    _rootNode = reader._rootNode = reader.parseFBX(&fbxFile);
    _geometry = reader.extractFBXGeometry({}, _modelURL.toString());
    _textureContent = reader._textureContent;
}

void FBXBaker::rewriteAndBakeSceneModels() {
    unsigned int meshIndex = 0;
    bool hasDeformers { false };
    for (FBXNode& rootChild : _rootNode.children) {
        if (rootChild.name == "Objects") {
            for (FBXNode& objectChild : rootChild.children) {
                if (objectChild.name == "Deformer") {
                    hasDeformers = true;
                    break;
                }
            }
        }
        if (hasDeformers) {
            break;
        }
    }
    for (FBXNode& rootChild : _rootNode.children) {
        if (rootChild.name == "Objects") {
            for (FBXNode& objectChild : rootChild.children) {
                if (objectChild.name == "Geometry") {

                    // TODO Pull this out of _geometry instead so we don't have to reprocess it
                    auto extractedMesh = FBXReader::extractMesh(objectChild, meshIndex, false);
                    
                    // Callback to get MaterialID
                    getMaterialIDCallback materialIDcallback = [=](int partIndex) {return extractedMesh.partMaterialTextures[partIndex].first;};
                    
                    // Compress mesh information and store in dracoMeshNode
                    FBXNode dracoMeshNode;
                    bool success = this->compressMesh(extractedMesh.mesh, hasDeformers, dracoMeshNode, materialIDcallback);
                    
                    // if bake fails - return, if there were errors and continue, if there were warnings.
                    if (!success) {
                        if (hasErrors()) {
                            return;
                        } else if (hasWarnings()) {
                            continue;
                        }
                    }
                    
                    objectChild.children.push_back(dracoMeshNode);

                    static const std::vector<QString> nodeNamesToDelete {
                        // Node data that is packed into the draco mesh
                        "Vertices",
                        "PolygonVertexIndex",
                        "LayerElementNormal",
                        "LayerElementColor",
                        "LayerElementUV",
                        "LayerElementMaterial",
                        "LayerElementTexture",

                        // Node data that we don't support
                        "Edges",
                        "LayerElementTangent",
                        "LayerElementBinormal",
                        "LayerElementSmoothing"
                    };
                    auto& children = objectChild.children;
                    auto it = children.begin();
                    while (it != children.end()) {
                        auto begin = nodeNamesToDelete.begin();
                        auto end = nodeNamesToDelete.end();
                        if (find(begin, end, it->name) != end) {
                            it = children.erase(it);
                        } else {
                            ++it;
                        }
                    }
                }
            }
        }
    }
}

void FBXBaker::rewriteAndBakeSceneTextures() {
    using namespace image::TextureUsage;
    QHash<QString, image::TextureUsage::Type> textureTypes;

    // enumerate the materials in the extracted geometry so we can determine the texture type for each texture ID
    for (const auto& material : _geometry->materials) {
        if (material.normalTexture.isBumpmap) {
            textureTypes[material.normalTexture.id] = BUMP_TEXTURE;
        } else {
            textureTypes[material.normalTexture.id] = NORMAL_TEXTURE;
        }

        textureTypes[material.albedoTexture.id] = ALBEDO_TEXTURE;
        textureTypes[material.glossTexture.id] = GLOSS_TEXTURE;
        textureTypes[material.roughnessTexture.id] = ROUGHNESS_TEXTURE;
        textureTypes[material.specularTexture.id] = SPECULAR_TEXTURE;
        textureTypes[material.metallicTexture.id] = METALLIC_TEXTURE;
        textureTypes[material.emissiveTexture.id] = EMISSIVE_TEXTURE;
        textureTypes[material.occlusionTexture.id] = OCCLUSION_TEXTURE;
        textureTypes[material.lightmapTexture.id] = LIGHTMAP_TEXTURE;
    }

    // enumerate the children of the root node
    for (FBXNode& rootChild : _rootNode.children) {

        if (rootChild.name == "Objects") {

            // enumerate the objects
            auto object = rootChild.children.begin();
            while (object != rootChild.children.end()) {
                if (object->name == "Texture") {

                    // double check that we didn't get an abort while baking the last texture
                    if (shouldStop()) {
                        return;
                    }

                    // enumerate the texture children
                    for (FBXNode& textureChild : object->children) {

                        if (textureChild.name == "RelativeFilename") {
                            QString fbxTextureFileName { textureChild.properties.at(0).toByteArray() };
                            
                            // Callback to get texture type
                            getTextureTypeCallback textureTypeCallback = [=]() {
                                // grab the ID for this texture so we can figure out the
                                // texture type from the loaded materials
                                auto textureID{ object->properties[0].toByteArray() };
                                auto textureType = textureTypes[textureID];
                                return textureType;
                            };

                            // Compress the texture information and return the new filename to be added into the FBX scene
                            QByteArray* bakedTextureFile = this->compressTexture(fbxTextureFileName, textureTypeCallback);

                            // If no errors or warnings have occurred during texture compression add the filename to the FBX scene
                            if (bakedTextureFile) {
                                textureChild.properties[0] = *bakedTextureFile;
                            } else {
                                // if bake fails - return, if there were errors and continue, if there were warnings.
                                if (hasErrors()) {
                                    return;
                                } else if (hasWarnings()) {
                                    continue;
                                }
                            }
                        }
                    }

                    ++object;

                } else if (object->name == "Video") {
                    // this is an embedded texture, we need to remove it from the FBX
                    object = rootChild.children.erase(object);
                } else {
                    ++object;
                }
            }
        }
    }
}

void FBXBaker::exportScene() {
    // save the relative path to this FBX inside our passed output folder
    auto fileName = _modelURL.fileName();
    auto baseName = fileName.left(fileName.lastIndexOf('.'));
    auto bakedFilename = baseName + BAKED_FBX_EXTENSION;

    _bakedFBXFilePath = _bakedOutputDir + "/" + bakedFilename;

    auto fbxData = FBXWriter::encodeFBX(_rootNode);

    QFile bakedFile(_bakedFBXFilePath);

    if (!bakedFile.open(QIODevice::WriteOnly)) {
        handleError("Error opening " + _bakedFBXFilePath + " for writing");
        return;
    }

    bakedFile.write(fbxData);

    _outputFiles.push_back(_bakedFBXFilePath);

    qCDebug(model_baking) << "Exported" << _modelURL << "with re-written paths to" << _bakedFBXFilePath;
}
