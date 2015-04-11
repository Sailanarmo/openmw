#include "lightmanager.hpp"

#include <osg/NodeVisitor>
#include <osg/Geode>

#include <osgUtil/CullVisitor>

#include <components/sceneutil/util.hpp>

#include <boost/functional/hash.hpp>

#include <iostream>
#include <stdexcept>

namespace SceneUtil
{

    class LightStateAttribute : public osg::Light
    {
    public:
        LightStateAttribute() {}

        LightStateAttribute(const LightStateAttribute& light,const osg::CopyOp& copyop=osg::CopyOp::SHALLOW_COPY)
            : osg::Light(light,copyop) {}

        LightStateAttribute(const osg::Light& light,const osg::CopyOp& copyop=osg::CopyOp::SHALLOW_COPY)
            : osg::Light(light,copyop) {}


        META_StateAttribute(NifOsg, LightStateAttribute, osg::StateAttribute::LIGHT)

        virtual void apply(osg::State& state) const
        {
            osg::Matrix modelViewMatrix = state.getModelViewMatrix();

            // FIXME: we shouldn't have to apply the modelview matrix after every light
            // this could be solvable by having the LightStateAttribute contain all lights instead of just one.
            state.applyModelViewMatrix(state.getInitialViewMatrix());

            osg::Light::apply(state);

            state.setGlobalDefaultAttribute(this);

            state.applyModelViewMatrix(modelViewMatrix);
        }
    };

    class CullCallback : public osg::NodeCallback
    {
    public:
        CullCallback()
            : mLightManager(NULL)
        {}
        CullCallback(const CullCallback& copy, const osg::CopyOp& copyop = osg::CopyOp::SHALLOW_COPY)
            : osg::Object(copy, copyop), osg::NodeCallback(copy, copyop), mLightManager(copy.mLightManager)
        {}
        CullCallback(LightManager* lightManager)
            : mLightManager(lightManager)
        {}

        META_Object(NifOsg, CullCallback)

        void operator()(osg::Node* node, osg::NodeVisitor* nv)
        {
            osgUtil::CullVisitor* cv = static_cast<osgUtil::CullVisitor*>(nv);

            mLightManager->prepareForCamera(cv->getCurrentCamera());

            // Possible optimizations:
            // - cull list of lights by the camera frustum
            // - organize lights in a quad tree

            const std::vector<LightManager::LightSourceTransform>& lights = mLightManager->getLights();

            if (lights.size())
            {
                // we do the intersections in view space
                osg::BoundingSphere nodeBound = node->getBound();
                osg::Matrixf mat = *cv->getModelViewMatrix();
                transformBoundingSphere(mat, nodeBound);

                std::vector<int> lightList;
                for (unsigned int i=0; i<lights.size(); ++i)
                {
                    const LightManager::LightSourceTransform& l = lights[i];
                    if (l.mViewBound.intersects(nodeBound))
                        lightList.push_back(i);
                }

                if (lightList.empty())
                {
                    traverse(node, nv);
                    return;
                }

                if (lightList.size() > 8)
                {
                    //std::cerr << "More than 8 lights!" << std::endl;

                    // TODO: sort lights by certain criteria

                    while (lightList.size() > 8)
                        lightList.pop_back();
                }

                osg::ref_ptr<osg::StateSet> stateset = mLightManager->getLightListStateSet(lightList);

                cv->pushStateSet(stateset);

                traverse(node, nv);

                cv->popStateSet();
            }
            else
                traverse(node, nv);
        }

    private:
        LightManager* mLightManager;
    };

    class AttachCullCallbackVisitor : public osg::NodeVisitor
    {
    public:
        AttachCullCallbackVisitor(LightManager* lightManager)
            : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
            , mLightManager(lightManager)
        {
        }

        virtual void apply(osg::Geode &geode)
        {
            if (!geode.getNumParents())
                return;

            // Not working on a Geode. Drawables are not normal children of the Geode, the traverse() call
            // does not traverse the drawables, so the push/pop in the CullCallback has no effect
            // Should be no longer an issue with osg trunk
            osg::Node* parent = geode.getParent(0);
            parent->addCullCallback(new CullCallback(mLightManager));
        }

    private:
        LightManager* mLightManager;
    };

    // Set on a LightSource. Adds the light source to its light manager for the current frame.
    // This allows us to keep track of the current lights in the scene graph without tying creation & destruction to the manager.
    class CollectLightCallback : public osg::NodeCallback
    {
    public:
        CollectLightCallback()
            : mLightManager(0) { }

        CollectLightCallback(const CollectLightCallback& copy, const osg::CopyOp& copyop)
            : osg::NodeCallback(copy, copyop)
            , mLightManager(0) { }

        META_Object(SceneUtil, SceneUtil::CollectLightCallback)

        virtual void operator()(osg::Node* node, osg::NodeVisitor* nv)
        {
            if (!mLightManager)
            {
                for (unsigned int i=0;i<nv->getNodePath().size(); ++i)
                {
                    if (LightManager* lightManager = dynamic_cast<LightManager*>(nv->getNodePath()[i]))
                    {
                        mLightManager = lightManager;
                        break;
                    }
                }
                if (!mLightManager)
                    throw std::runtime_error("can't find parent LightManager");
            }

            mLightManager->addLight(static_cast<LightSource*>(node), osg::computeLocalToWorld(nv->getNodePath()));

            traverse(node, nv);
        }

    private:
        LightManager* mLightManager;
    };

    // Set on a LightManager. Clears the data from the previous frame.
    class LightManagerUpdateCallback : public osg::NodeCallback
    {
    public:
        LightManagerUpdateCallback()
            { }

        LightManagerUpdateCallback(const LightManagerUpdateCallback& copy, const osg::CopyOp& copyop)
            : osg::NodeCallback(copy, copyop)
            { }

        META_Object(SceneUtil, SceneUtil::LightManagerUpdateCallback)

        virtual void operator()(osg::Node* node, osg::NodeVisitor* nv)
        {
            LightManager* lightManager = static_cast<LightManager*>(node);
            lightManager->update();

            traverse(node, nv);
        }
    };

    LightManager::LightManager()
        : mLightsInViewSpace(false)
        , mDecorated(false)
    {
        setUpdateCallback(new LightManagerUpdateCallback);
    }

    LightManager::LightManager(const LightManager &copy, const osg::CopyOp &copyop)
        : osg::Group(copy, copyop)
        , mLightsInViewSpace(false)
        , mDecorated(copy.mDecorated)
    {

    }

    void LightManager::decorateGeodes()
    {
        AttachCullCallbackVisitor visitor(this);
        accept(visitor);
    }

    void LightManager::update()
    {
        mLightsInViewSpace = false;
        mLights.clear();
        mStateSetCache.clear();

        if (!mDecorated)
        {
            decorateGeodes();
            mDecorated = true;
        }
    }

    void LightManager::addLight(LightSource* lightSource, osg::Matrix worldMat)
    {
        LightSourceTransform l;
        l.mLightSource = lightSource;
        l.mWorldMatrix = worldMat;
        mLights.push_back(l);
    }

    void LightManager::prepareForCamera(osg::Camera *cam)
    {
        // later on we need to store this per camera
        if (!mLightsInViewSpace)
        {
            for (std::vector<LightSourceTransform>::iterator it = mLights.begin(); it != mLights.end(); ++it)
            {
                LightSourceTransform& l = *it;
                osg::Matrix worldViewMat = l.mWorldMatrix * cam->getViewMatrix();
                l.mViewBound = osg::BoundingSphere(osg::Vec3f(0,0,0), l.mLightSource->getRadius());
                transformBoundingSphere(worldViewMat, l.mViewBound);
            }
            mLightsInViewSpace = true;
        }
    }

    osg::ref_ptr<osg::StateSet> LightManager::getLightListStateSet(const LightList &lightList)
    {
        // possible optimization: return a StateSet containing all requested lights plus some extra lights (if a suitable one exists)
        size_t hash = 0;
        for (unsigned int i=0; i<lightList.size();++i)
            boost::hash_combine(hash, lightList[i]);

        LightStateSetMap::iterator found = mStateSetCache.find(hash);
        if (found != mStateSetCache.end())
            return found->second;
        else
        {
            osg::ref_ptr<osg::StateSet> stateset (new osg::StateSet);
            for (unsigned int i=0; i<lightList.size();++i)
            {
                int lightIndex = lightList[i];
                osg::Light* light = mLights[lightIndex].mLightSource->getLight();

                osg::ref_ptr<LightStateAttribute> clonedLight = new LightStateAttribute(*light, osg::CopyOp::DEEP_COPY_ALL);
                clonedLight->setPosition(mLights[lightIndex].mWorldMatrix.preMult(light->getPosition()));

                clonedLight->setLightNum(i);

                // don't use setAttributeAndModes, that does not support light indices!
                stateset->setAttribute(clonedLight, osg::StateAttribute::ON);
                stateset->setAssociatedModes(clonedLight, osg::StateAttribute::ON);
            }
            mStateSetCache.insert(std::make_pair(hash, stateset));
            return stateset;
        }
    }

    const std::vector<LightManager::LightSourceTransform>& LightManager::getLights() const
    {
        return mLights;
    }

    LightSource::LightSource()
        : mRadius(0.f)
    {
        setUpdateCallback(new CollectLightCallback);
    }

}