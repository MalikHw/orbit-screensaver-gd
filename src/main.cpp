#include <Geode/Geode.hpp>
#include <Geode/modify/CCScene.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/modify/LevelEditorLayer.hpp>
#include <Geode/modify/CCTouchDispatcher.hpp>
#include <Geode/modify/CCKeyboardDispatcher.hpp>

#include "BlurAPI.hpp"
#include <box2d/box2d.h>

using namespace geode::prelude;
using namespace cocos2d;

static float idleTimer = 0.f;
static bool ssActive = false;
static bool pauseOpen = false;

struct PhysItem {
    b2Body* phys;
    CCNode* vis;
    float radius;
    int itemType;
    bool isPlayerCube;
};

class ScreensaverStuff : public CCLayerColor {
    b2World* physWorld;
    b2Body* floorBody;
    std::vector<PhysItem> items;
    
    float elapsedTime;
    int totalSpawned;
    bool hasSpawnedPlayer;
    bool isDraining;
    bool isFilledUp;
    float dt_accum;
    float waitTime;
    float nextSpawnAt;
    
    int maxItems;
    float simSpeed;
    int cubeChance;
    bool noGround;
    int spawnInterval;
    
    std::string bgType;
    std::string bgFitMode;
    ccColor3B bgCol;
    int bgAlpha;
    
    CCSprite* bgImg;
    
    float fadeTime;
    bool fadeDone;
    bool killed;
    CCSize winSize;
    
    static const int orbIds[11];
    static const float orbSizes[11];
    
    void makeEdge(float x1, float y1, float x2, float y2, b2Body** out) {
        b2BodyDef bd;
        bd.type = b2_staticBody;
        auto b = physWorld->CreateBody(&bd);
        
        b2EdgeShape edge;
        edge.SetTwoSided({x1/40.f, y1/40.f}, {x2/40.f, y2/40.f});
        
        b2FixtureDef fd;
        fd.shape = &edge;
        fd.restitution = 0.5f;
        fd.friction = 0.7f;
        b->CreateFixture(&fd);
        
        if(out) *out = b;
    }
    
    void loadBgImg() {
        if(bgImg) {
            bgImg->removeFromParentAndCleanup(true);
            bgImg = nullptr;
        }
        
        if(bgType != "image") return;
        
        auto imgPath = Mod::get()->getSettingValue<std::filesystem::path>("bg-image");
        if(imgPath.empty() || !std::filesystem::exists(imgPath)) return;
        
        auto tex = CCTextureCache::get()->addImage(imgPath.string().c_str(), false);
        if(!tex) return;
        
        bgImg = CCSprite::createWithTexture(tex);
        if(!bgImg) return;
        
        bgImg->setAnchorPoint({0.5f, 0.5f});
        bgImg->setPosition(winSize.width/2, winSize.height/2);
        
        fitBgImg();
        this->addChild(bgImg, -1);
    }
    
    void fitBgImg() {
        if(!bgImg) return;
        
        auto texSz = bgImg->getTextureRect().size;
        if(texSz.width <= 0 || texSz.height <= 0) return;
        
        float scaleX = winSize.width / texSz.width;
        float scaleY = winSize.height / texSz.height;
        
        if(bgFitMode == "stretch") {
            bgImg->setScaleX(scaleX);
            bgImg->setScaleY(scaleY);
        } else {
            float s = std::max(scaleX, scaleY);
            bgImg->setScale(s);
        }
        
        bgImg->setOpacity(bgAlpha);
    }
    
    void dropItem(bool forcePlayer = false) {
        float r;
        int orbType = -1;
        bool isPlayer = forcePlayer;
        bool isBox = false;
        
        if(isPlayer) {
            r = 35.f / 2.f;
            isBox = true;
        } else {
            orbType = std::rand() % 11;
            isBox = (orbType == 10);
            r = orbSizes[orbType] / 2.f;
        }
        
        b2BodyDef bd;
        bd.type = b2_dynamicBody;
        bd.angle = (std::rand() % 360) * M_PI / 180.f;
        
        if(isPlayer) {
            bd.position.Set(
                (float)(std::rand() % (int)std::max(1.f, winSize.width)) / 40.f,
                -(200.f + std::rand() % 800) / 40.f
            );
        } else {
            bd.position.Set(
                (winSize.width*0.1f + (std::rand() % (int)std::max(1.f, winSize.width*0.8f))) / 40.f,
                -250.f / 40.f
            );
        }
        
        auto body = physWorld->CreateBody(&bd);
        
        b2FixtureDef fd;
        b2CircleShape circ;
        b2PolygonShape poly;
        
        fd.density = 1.f;
        fd.restitution = 0.5f;
        fd.friction = isPlayer ? 0.7f : 1.f;
        
        if(isBox) {
            poly.SetAsBox(r/40.f, r/40.f);
            fd.shape = &poly;
        } else {
            circ.m_radius = r/40.f;
            fd.shape = &circ;
        }
        
        body->CreateFixture(&fd);
        
        if(!isPlayer) {
            body->ApplyLinearImpulse({(10 - std::rand()%21)*0.05f, 0.f}, body->GetWorldCenter(), true);
        }
        
        CCNode* visNode = nullptr;
        
        if(isPlayer) {
            auto gm = GameManager::get();
            auto sp = SimplePlayer::create(gm->getPlayerFrame());
            if(sp) {
                sp->setColors(gm->colorForIdx(gm->getPlayerColor()), 
                             gm->colorForIdx(gm->getPlayerColor2()));
                if(gm->getPlayerGlow())
                    sp->setGlowOutline(gm->colorForIdx(gm->getPlayerGlowColor()));
                sp->setScale(1.f);
                sp->setPosition(-9999, -9999);
                this->addChild(sp, 3);
                visNode = sp;
            }
        } else {
            auto obj = GameObject::createWithKey(orbIds[orbType]);
            if(obj) {
                obj->setScale(1.f);
                obj->setPosition(-9999, -9999);
                this->addChild(obj, 2);
                visNode = obj;
            }
        }
        
        items.push_back({body, visNode, r, orbType, isPlayer});
    }
    
    void reset() {
        maxItems = (int)Mod::get()->getSettingValue<int64_t>("orb-count");
        cubeChance = (int)Mod::get()->getSettingValue<int64_t>("cube-chance");
        noGround = Mod::get()->getSettingValue<bool>("no-ground");
        simSpeed = (float)Mod::get()->getSettingValue<int64_t>("orb-speed") / 10.f;
        spawnInterval = std::max(1, (int)(20.f/simSpeed));
        if(maxItems < 1) maxItems = 1;
        
        elapsedTime = 0.f;
        totalSpawned = 0;
        hasSpawnedPlayer = false;
        isFilledUp = false;
        isDraining = false;
        dt_accum = 0.f;
        waitTime = 5.f + (std::rand()%1001)/1000.f;
        nextSpawnAt = 0.f;
        
        for(auto& item : items) {
            if(item.vis) item.vis->removeFromParentAndCleanup(true);
        }
        items.clear();
        
        delete physWorld;
        physWorld = nullptr;
        floorBody = nullptr;
        
        physWorld = new b2World({0.f, 9.8f*simSpeed*3.f});
        
        float W = winSize.width;
        float H = winSize.height;
        
        makeEdge(0,0, 0,H, nullptr);
        makeEdge(W,0, W,H, nullptr);
        
        if(!noGround)
            makeEdge(0,H, W,H, &floorBody);
    }
    
    bool init() override {
        bgType = Mod::get()->getSettingValue<std::string>("bg-type");
        
        if(bgType == "blur" && BlurAPI::isBlurAPIEnabled()) {
            // blur mode
        } else if(bgType == "image") {
            // image mode
        } else {
            bgType = "color";
        }
        
        bgCol = Mod::get()->getSettingValue<ccColor3B>("bg-color");
        bgAlpha = (int)Mod::get()->getSettingValue<int64_t>("bg-opacity");
        bgFitMode = Mod::get()->getSettingValue<std::string>("bg-fit");
        
        GLubyte initAlpha = (bgType == "color") ? 0 : 0;
        if(!CCLayerColor::initWithColor({bgCol.r, bgCol.g, bgCol.b, initAlpha}))
            return false;
        
        winSize = CCDirector::get()->getWinSize();
        this->setContentSize(winSize);
        this->setPosition(CCPointZero);
        
        if(bgType == "blur")
            BlurAPI::addBlur(this);
        
        loadBgImg();
        
        this->setTouchEnabled(true);
        this->setTouchMode(kCCTouchesOneByOne);
        this->setTouchPriority(-9999);
        this->setKeypadEnabled(true);
        this->setKeyboardEnabled(true);
#ifdef GEODE_IS_DESKTOP
        this->setMouseEnabled(true);
#endif
        
        this->scheduleUpdate();
        std::srand((unsigned)std::time(nullptr));
        
        fadeTime = 0.f;
        fadeDone = false;
        killed = false;
        
        reset();
        return true;
    }
    
    void update(float dt) override {
        if(killed) return;
        
        if(!fadeDone) {
            fadeTime += dt;
            float t = std::min(fadeTime/0.5f, 1.f);
            if(t >= 1.f) fadeDone = true;
            
            if(bgType == "blur") {
                this->setOpacity((GLubyte)(180*t));
            } else if(bgType == "color") {
                this->setOpacity((GLubyte)(bgAlpha*t));
            } else {
                this->setOpacity(0);
                if(bgImg)
                    bgImg->setOpacity((GLubyte)(bgAlpha*t));
            }
            return;
        }
        
        float W = winSize.width;
        float H = winSize.height;
        
        elapsedTime += dt;
        
        // spawn items based on time intervals
        float spawnDelay = (float)spawnInterval / 60.f; // convert to seconds
        while(totalSpawned < maxItems && elapsedTime >= nextSpawnAt) {
            dropItem(false);
            totalSpawned++;
            nextSpawnAt = elapsedTime + spawnDelay;
        }
        
        if(!hasSpawnedPlayer && totalSpawned >= maxItems/2) {
            hasSpawnedPlayer = true;
            if((std::rand()%100) < cubeChance)
                dropItem(true);
        }
        
        if(!noGround && !isFilledUp && totalSpawned >= maxItems) {
            float totalSpawnTime = (float)maxItems * spawnDelay;
            if(elapsedTime >= totalSpawnTime + waitTime) {
                isFilledUp = true;
                isDraining = true;
                if(floorBody) {
                    physWorld->DestroyBody(floorBody);
                    floorBody = nullptr;
                }
            }
        }
        
        if(!noGround && isDraining) {
            bool allOff = true;
            for(auto& item : items) {
                if(item.phys->GetPosition().y*40.f < H+300.f) {
                    allOff = false;
                    break;
                }
            }
            if(allOff) {
                reset();
                return;
            }
        }
        
        if(noGround && elapsedTime > (float)maxItems * spawnDelay + 8.f) {
            reset();
            return;
        }
        
        float STEP = 1.f/60.f;
        dt_accum += dt;
        while(dt_accum >= STEP) {
            physWorld->Step(STEP, 8, 3);
            dt_accum -= STEP;
        }
        
        for(auto& item : items) {
            if(!item.vis) continue;
            auto pos = item.phys->GetPosition();
            item.vis->setPosition(pos.x*40.f, H - pos.y*40.f);
            item.vis->setRotation(item.phys->GetAngle() * 180.f/M_PI);
        }
    }
    
public:
    static ScreensaverStuff* create() {
        auto ret = new ScreensaverStuff();
        if(ret && ret->init()) {
            ret->autorelease();
            ret->setTag(2047);
            return ret;
        }
        CC_SAFE_DELETE(ret);
        return nullptr;
    }
    
    void kill() {
        if(killed) return;
        killed = true;
        this->stopAllActions();
        this->unscheduleAllSelectors();
        this->removeFromParentAndCleanup(true);
        ssActive = false;
        idleTimer = 0.f;
    }
    
    bool ccTouchBegan(CCTouch*, CCEvent*) override {
        kill();
        return true;
    }
    
    void keyDown(enumKeyCodes, double) override {
        kill();
    }
    
    void keyBackClicked() override {
        kill();
    }
    
#ifdef GEODE_IS_DESKTOP
    void scrollWheel(float, float) override {
        kill();
    }
#endif
    
    ~ScreensaverStuff() override {
        if(bgType == "blur")
            BlurAPI::removeBlur(this);
        delete physWorld;
    }
};

const int ScreensaverStuff::orbIds[11] = {36, 84, 141, 1022, 1330, 1333, 1704, 1751, 3004, 3027, 1594};
const float ScreensaverStuff::orbSizes[11] = {32.3f, 33.2f, 33.6f, 31.96f, 36.5f, 34.9f, 41.f, 41.f, 41.f, 39.4f, 30.8f};

class $modify(CCScene) {
    bool init() {
        if(!CCScene::init()) return false;
        this->schedule(schedule_selector($modify(CCScene)::checkIdle), 0.f);
        return true;
    }
    
    void checkIdle(float dt) {
        // don't run during editor or active gameplay
        auto scene = CCDirector::get()->getRunningScene();
        if(!scene) return;
        
        bool inEditor = scene->getChildByType<LevelEditorLayer>(0) != nullptr;
        bool inPlay = scene->getChildByType<PlayLayer>(0) != nullptr;
        
        // reset timer if in editor, or if in active gameplay (not paused)
        if(inEditor || (inPlay && !pauseOpen) || ssActive) {
            idleTimer = 0.f;
            return;
        }
        
        idleTimer += dt;
        float timeout = (float)Mod::get()->getSettingValue<int64_t>("idle-timeout");
        
        if(idleTimer < timeout) return;
        
        idleTimer = 0.f;
        ssActive = true;
        
        auto ss = ScreensaverStuff::create();
        if(ss) {
            ss->setZOrder(9999);
            this->addChild(ss);
        } else {
            ssActive = false;
        }
    }
};

class $modify(CCTouchDispatcher) {
    void touches(CCSet* t, CCEvent* e, unsigned int type) {
        CCTouchDispatcher::touches(t, e, type);
        if(type == CCTOUCHBEGAN)
            idleTimer = 0.f;
    }
};

class $modify(CCKeyboardDispatcher) {
    bool dispatchKeyboardMSG(enumKeyCodes key, bool down, bool repeat, double ts) {
        bool result = CCKeyboardDispatcher::dispatchKeyboardMSG(key, down, repeat, ts);
        
        if(down && !repeat) {
            idleTimer = 0.f;
            auto scene = CCDirector::get()->getRunningScene();
            if(scene) {
                auto ss = scene->getChildByTag(2047);
                if(ss)
                    static_cast<ScreensaverStuff*>(ss)->kill();
            }
        }
        
        return result;
    }
};

class $modify(PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();
        pauseOpen = true;
        idleTimer = 0.f;
    }
    
    void onQuit(CCObject* sender) {
        pauseOpen = false;
        idleTimer = 0.f;
        PauseLayer::onQuit(sender);
    }
    
    void onResume(CCObject* sender) {
        pauseOpen = false;
        idleTimer = 0.f;
        
        auto scene = CCDirector::get()->getRunningScene();
        if(scene) {
            auto ss = scene->getChildByTag(2047);
            if(ss)
                static_cast<ScreensaverStuff*>(ss)->kill();
        }
        
        PauseLayer::onResume(sender);
    }
};

class $modify(LevelEditorLayer) {
    bool init(GJGameLevel* lvl, bool unk) {
        idleTimer = 0.f;
        return LevelEditorLayer::init(lvl, unk);
    }
};
