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

// global state
static float idleTimer = 0.f; static bool ssActive = false; static bool pauseOpen = false;

// physics item struct
struct PhysItem { b2Body* phys; CCNode* vis; float radius; int itemType; bool isPlayerCube;};

// screensaverLayer, as the class says
class ScreensaverLayer : public CCLayerColor {
    // physics/scene-related state
    b2World* physWorld = nullptr;
    b2Body* ground = nullptr;
    std::vector<PhysItem> items;

    // timing/gp
    float elapsed = 0.f;
    int itemsMade = 0;
    bool playerIn = false;
    bool draining = false;
    bool filled = false;
    float physicsLag = 0.f;
    float pauseBeforeDrain = 0.f;
    float nextSpawn = 0.f;

    // configurations
    int maxItems = 0;
    float physicsSpeed = 1.f;
    int playerChance = 0;
    bool allowDrop = false;
    int spawnTicks = 1;

    // background
    std::string bgType;
    std::string bgFit;
    ccColor3B bgCol;
    int bgAlpha = 128;
    CCSprite* bgSprite = nullptr;

    // animation/transitions
    float fadeIn = 0.f;
    bool doneFade = false;
    bool killed = false;
    CCSize winSize;

    // orb lookup tables
    static const int orbIds[11];
    static const float orbSizes[11];

    // create static/fixed line in the physics world
    void makeEdge(float x1, float y1, float x2, float y2, b2Body** out) {
        b2BodyDef bd;
        bd.type = b2_staticBody;
        b2Body* edgeBody = physWorld->CreateBody(&bd);

        b2EdgeShape edge;
        edge.SetTwoSided({x1/40.f, y1/40.f}, {x2/40.f, y2/40.f});
        b2FixtureDef fd;
        fd.shape = &edge;
        fd.restitution = 0.5f;
        fd.friction = 0.7f;
        edgeBody->CreateFixture(&fd);
        if(out) *out = edgeBody;
    }

    // bg image load
    void loadBackgroundImage() {
        if(bgSprite) {
            bgSprite->removeFromParentAndCleanup(true);
            bgSprite = nullptr;
        }
        if(bgType != "image") return;
        auto imgPath = Mod::get()->getSettingValue<std::filesystem::path>("image-path");
        if(imgPath.empty() || !std::filesystem::exists(imgPath)) return;
        auto tex = CCTextureCache::get()->addImage(imgPath.string().c_str(), false);
        if(!tex) return;
        bgSprite = CCSprite::createWithTexture(tex);
        if(!bgSprite) return;
        bgSprite->setAnchorPoint({0.5f, 0.5f});
        bgSprite->setPosition({winSize.width/2, winSize.height/2});
        fitBackgroundImage();
        this->addChild(bgSprite, -1);
    }
    void fitBackgroundImage() {
        if(!bgSprite) return;
        auto sz = bgSprite->getTextureRect().size;
        if(sz.width <= 0 || sz.height <= 0) return;
        float sx = winSize.width / sz.width;
        float sy = winSize.height / sz.height;
        if(bgFit == "stretch") {
            bgSprite->setScaleX(sx);
            bgSprite->setScaleY(sy);
        } else {
            float s = std::max(sx, sy);
            bgSprite->setScale(s);
        }
        bgSprite->setOpacity(bgAlpha);
    }

    // spawn something
    void addItem(bool forcePlayer = false) {
        float rad;
        int orbIdx = -1;
        bool spawnPlayer = forcePlayer;
        bool isBox = false;

        if(spawnPlayer) {
            rad = 17.5f;
            isBox = true;
        } else {
            orbIdx = std::rand() % 11;
            isBox = (orbIdx == 10);
            rad = orbSizes[orbIdx] / 2.f;
        }

        b2BodyDef bd;
        bd.type = b2_dynamicBody;
        bd.angle = (std::rand() % 360) * static_cast<float>(M_PI) / 180.f;

        // random spawn position for variety
        if(spawnPlayer) {
            bd.position.Set(
                (float)(std::rand() % static_cast<int>(std::max(1.f, winSize.width))) / 40.f,
                -(200.f + std::rand() % 800) / 40.f
            );
        } else {
            bd.position.Set(
                (winSize.width*0.1f + std::rand() % static_cast<int>(std::max(1.f, winSize.width*0.8f))) / 40.f,
                -250.f / 40.f
            );
        }
        b2Body* body = physWorld->CreateBody(&bd);

        b2FixtureDef fd;
        b2CircleShape circ;
        b2PolygonShape poly;
        fd.density = 1.0f;
        fd.restitution = 0.5f;
        fd.friction = spawnPlayer ? 0.7f : 1.0f;
        if(isBox) {
            poly.SetAsBox(rad/40.f, rad/40.f); // Box for player
            fd.shape = &poly;
        } else {
            circ.m_radius = rad/40.f; // Circle for orb
            fd.shape = &circ;
        }
        body->CreateFixture(&fd);

        // give orbs a push
        if(!spawnPlayer) {
            body->ApplyLinearImpulse({(10 - std::rand()%21)*0.05f, 0.f}, body->GetWorldCenter(), true);
        }

        // visual
        CCNode* visual = nullptr;
        if(spawnPlayer) {
            auto gm = GameManager::get();
            auto sp = SimplePlayer::create(gm->getPlayerFrame());
            if(sp) {
                sp->setColors(gm->colorForIdx(gm->getPlayerColor()), gm->colorForIdx(gm->getPlayerColor2()));
                if(gm->getPlayerGlow())
                    sp->setGlowOutline(gm->colorForIdx(gm->getPlayerGlowColor()));
                sp->setScale(1.0f);
                sp->setPosition({-9999.f, -9999.f});
                this->addChild(sp, 3);
                visual = sp;
            }
        } else {
            auto obj = GameObject::createWithKey(orbIds[orbIdx]);
            if(obj) {
                obj->setScale(1.0f);
                obj->setPosition({-9999.f, -9999.f});
                this->addChild(obj, 2);
                visual = obj;
            }
        }
        items.push_back({body, visual, rad, orbIdx, spawnPlayer});
    }

    // reset
    void fullReset() {
        maxItems = static_cast<int>(Mod::get()->getSettingValue<int64_t>("item-count"));
        playerChance = static_cast<int>(Mod::get()->getSettingValue<int64_t>("player-chance"));
        allowDrop = Mod::get()->getSettingValue<bool>("infinite-fall");
        physicsSpeed = static_cast<float>(Mod::get()->getSettingValue<int64_t>("physics-speed")) / 10.f;
        spawnTicks = std::max(1, static_cast<int>(20.f/physicsSpeed));
        if(maxItems < 1) maxItems = 1;

        elapsed = 0.f;
        itemsMade = 0;
        playerIn = false;
        filled = false;
        draining = false;
        physicsLag = 0.f;
        pauseBeforeDrain = 5.f + (std::rand()%1001)/1000.f;
        nextSpawn = 0.f;

        // clean up existing visuals
        for(auto& item : items)
            if(item.vis) item.vis->removeFromParentAndCleanup(true);
        items.clear();

        delete physWorld;
        physWorld = nullptr;
        ground = nullptr;
        physWorld = new b2World({0.f, 9.8f*physicsSpeed*3.f});

        float W = winSize.width, H = winSize.height;
        makeEdge(0, 0, 0, H, nullptr);
        makeEdge(W, 0, W, H, nullptr);
        if(!allowDrop)
            makeEdge(0, H, W, H, &ground);
    }

    // layer init
    bool init() override {
        bgType = Mod::get()->getSettingValue<std::string>("background-mode");
        if(bgType != "blur" && bgType != "image") bgType = "color";
        bgCol = Mod::get()->getSettingValue<ccColor3B>("overlay-color");
        bgAlpha = static_cast<int>(Mod::get()->getSettingValue<int64_t>("overlay-opacity"));
        bgFit = Mod::get()->getSettingValue<std::string>("image-fit");

        GLubyte initAlpha = 0;
        if(!CCLayerColor::initWithColor({bgCol.r, bgCol.g, bgCol.b, initAlpha}))
            return false;
        winSize = CCDirector::get()->getWinSize();
        this->setContentSize(winSize);
        this->setPosition(CCPointZero);

        // setup blur if bgType == blur
        if(bgType == "blur" && BlurAPI::isBlurAPIEnabled())
            BlurAPI::addBlur(this);

        loadBackgroundImage();

        this->setTouchEnabled(true);
        this->setTouchMode(kCCTouchesOneByOne);
        this->setTouchPriority(-9999);
        this->setKeypadEnabled(true);
        this->setKeyboardEnabled(true);
        this->setMouseEnabled(true);

        this->scheduleUpdate();
        std::srand((unsigned)std::time(nullptr));
        fadeIn = 0.f;
        doneFade = false;
        killed = false;
        fullReset();
        return true;
    }

    // physics bullshit
    void update(float dt) override {
        if(killed) return;

        if(!doneFade) {
            fadeIn += dt;
            float t = std::min(fadeIn/0.5f, 1.f);
            if(t >= 1.f) doneFade = true;
            if(bgType == "blur") {
                setOpacity(static_cast<GLubyte>(180*t));
            } else if(bgType == "color") {
                setOpacity(static_cast<GLubyte>(bgAlpha*t));
            } else {
                setOpacity(0);
                if(bgSprite)
                    bgSprite->setOpacity(static_cast<GLubyte>(bgAlpha*t));
            }
            return;
        }

        float W = winSize.width, H = winSize.height;
        elapsed += dt;
        float spawnDelay = static_cast<float>(spawnTicks) / 60.f;

        while(itemsMade < maxItems && elapsed >= nextSpawn) {
            addItem(false);
            itemsMade++;
            nextSpawn = elapsed + spawnDelay;
        }

        if(!playerIn && itemsMade >= maxItems/2) {
            playerIn = true;
            if((std::rand()%100) < playerChance)
                addItem(true);
        }

        if(!allowDrop && !filled && itemsMade >= maxItems) {
            float spawnTime = maxItems * spawnDelay;
            if(elapsed >= spawnTime + pauseBeforeDrain) {
                filled = true;
                draining = true;
                if(ground) {
                    physWorld->DestroyBody(ground);
                    ground = nullptr;
                }
            }
        }

        if(!allowDrop && draining) {
            bool allGone = true;
            for(const auto& item : items)
                if(item.phys->GetPosition().y*40.f < H + 300.f) allGone = false;
            if(allGone) {
                fullReset();
                return;
            }
        }

        if(allowDrop && elapsed > (float)maxItems*spawnDelay + 8.f) {
            fullReset();
            return;
        }

        const float STEP = 1.f/60.f;
        physicsLag += dt;
        while(physicsLag >= STEP) {
            physWorld->Step(STEP, 8, 3);
            physicsLag -= STEP;
        }

        for(auto& item : items) {
            if(!item.vis) continue;
            auto pos = item.phys->GetPosition();
            item.vis->setPosition(pos.x*40.f, H - pos.y*40.f);
            item.vis->setRotation(item.phys->GetAngle() * 180.f/M_PI);
        }
    }

public:
    static ScreensaverLayer* create() {
        auto ptr = new ScreensaverLayer();
        if(ptr && ptr->init()) {
            ptr->autorelease();
            ptr->setTag(2047);
            return ptr;
        }
        CC_SAFE_DELETE(ptr);
        return nullptr;
    }

    // dismiss screen
    void kill() {
        if(killed) return;
        killed = true;
        this->stopAllActions();
        this->unscheduleAllSelectors();
        this->removeFromParentAndCleanup(true);
        ssActive = false;
        idleTimer = 0.f;
    }

    // kill screensaver on input
    bool ccTouchBegan(CCTouch*, CCEvent*) override {
        kill();
        return true;
    }
    void keyDown(enumKeyCodes, double) override { kill(); }
    void keyBackClicked() override { kill(); }
    void scrollWheel(float, float) override { kill(); }

    ~ScreensaverLayer() override {
        if(bgType == "blur")
            BlurAPI::removeBlur(this);
        delete physWorld;
    }
};

const int ScreensaverLayer::orbIds[11] = {36, 84, 141, 1022, 1330, 1333, 1704, 1751, 3004, 3027, 1594};
const float ScreensaverLayer::orbSizes[11] = {32.3f, 33.2f, 33.6f, 31.96f, 36.5f, 34.9f, 41.f, 41.f, 41.f, 39.4f, 30.8f};


// idle logic
class $modify(OrbitScrsvrScene, CCScene) {
    bool init() {
        if(!CCScene::init()) return false;
        this->schedule(schedule_selector(OrbitScrsvrScene::checkIdle), 0.f);
        return true;
    }

    void checkIdle(float dt) {
        auto scene = CCDirector::get()->getRunningScene();
        if(!scene) return;
        bool inEditor = scene->getChildByType<LevelEditorLayer>(0) != nullptr;
        bool inPlay = scene->getChildByType<PlayLayer>(0) != nullptr;

        // only activate screensaver if not in active gameplay/editor, not already running
        if(inEditor || (inPlay && !pauseOpen) || ssActive) {
            idleTimer = 0.f;
            return;
        }

        idleTimer += dt;
        float timeout = static_cast<float>(Mod::get()->getSettingValue<int64_t>("wait-time"));
        if(idleTimer < timeout) return;

        idleTimer = 0.f;
        ssActive = true;

        auto ss = ScreensaverLayer::create();
        if(ss) {
            ss->setZOrder(9999);
            this->addChild(ss);
        } else {
            ssActive = false;
        }
    }
};

// any touch on the screen resets idle timer
class $modify(CCTouchDispatcher) {
    void touches(CCSet* t, CCEvent* e, unsigned int type) {
        CCTouchDispatcher::touches(t, e, type);
        if(type == CCTOUCHBEGAN)
            idleTimer = 0.f;
    }
};

// any key also resets idle timer and removes screensaver if here
class $modify(CCKeyboardDispatcher) {
    bool dispatchKeyboardMSG(enumKeyCodes key, bool down, bool repeat, double ts) {
        bool result = CCKeyboardDispatcher::dispatchKeyboardMSG(key, down, repeat, ts);
        if(down && !repeat) {
            idleTimer = 0.f;
            auto scene = CCDirector::get()->getRunningScene();
            if(scene) {
                auto ss = scene->getChildByTag(2047);
                if(ss)
                    static_cast<ScreensaverLayer*>(ss)->kill();
            }
        }
        return result;
    }
};

// when PauseLayer opens and closes AND maintain proper idle state
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
                static_cast<ScreensaverLayer*>(ss)->kill();
        }
        PauseLayer::onResume(sender);
    }
};

// reset idle timer
class $modify(LevelEditorLayer) {
    bool init(GJGameLevel* lvl, bool unk) {
        idleTimer = 0.f;
        return LevelEditorLayer::init(lvl, unk);
    }
};
