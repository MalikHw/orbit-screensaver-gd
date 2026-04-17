#include <Geode/Geode.hpp>
#include <Geode/modify/CCScene.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/CCTouchDispatcher.hpp>
#include <Geode/modify/CCKeyboardDispatcher.hpp>

#include "BlurAPI.hpp"
#include <box2d/box2d.h>

#include <vector>
#include <cmath>
#include <cstdlib>
#include <ctime>

using namespace geode::prelude;
using namespace cocos2d;

static bool  g_inLevel  = false;
static float g_idle     = 0.f;
static bool  g_active   = false;

static constexpr float PPM = 40.f;
static constexpr float FADETIME = 0.5f;

// fiksed
// 36,     84,   141,   1022,  1330,  1333,  1704,  1751,  3004,  3027,  1594
// yellow, blue, pink, green, black, red, dash, dash2, spider, teleport, toggle
static const int   ORB_IDS[11]   = { 36, 84, 141, 1022, 1330, 1333, 1704, 1751, 3004, 3027, 1594 };
static const float ORB_SIZE[11]  = { 32.3f, 33.2f, 33.6f, 31.96f, 36.5f, 34.9f, 41.f, 41.f, 41.f, 39.4f, 30.8f };
static constexpr float PLAYER_SIZE = 35.f;

enum class BgMode { Color, Blur };

class ScreensaverLayer : public CCLayerColor {
    struct PhysBall {
        b2Body* body = nullptr;
        CCNode* node = nullptr;
        float   r    = 0.f;
        int     idx  = 0;
        bool    cube = false;
    };

    // physics
    b2World* world  = nullptr;
    b2Body*  ground = nullptr;
    std::vector<PhysBall> balls;

    // cycle state
    int   tick       = 0;
    int   spawned    = 0;
    bool  gotPlayer  = false;
    bool  filled     = false;
    bool  draining   = false;
    float accumulator = 0.f;
    float drainWait  = 3.f;

    // settings (read each cycle)
    int   numBalls  = 120;
    float spd       = 1.f;
    int   cubePct   = 50;
    bool  noFloor   = false;
    int   dropEvery = 2;

    // bg
    BgMode    bgMode = BgMode::Color;
    ccColor4B bgCol  = {0,0,0,255};

    // fade
    float fadeT  = 0.f;
    bool  fadeOk = false;

    bool  dead = false;
    CCSize scr;

    b2Body* makeWall(float x1, float y1, float x2, float y2) {
        b2BodyDef bd; bd.type = b2_staticBody;
        auto* b = world->CreateBody(&bd);
        b2EdgeShape e;
        e.SetTwoSided({x1/PPM, y1/PPM}, {x2/PPM, y2/PPM});
        b2FixtureDef fd; fd.shape = &e; fd.restitution = 0.5f; fd.friction = 0.7f;
        b->CreateFixture(&fd);
        return b;
    }

    void spawnBall(float W, float H, bool forcePlayer = false) {
        float half;
        int orbIdx = -1;
        bool isPlayer = forcePlayer;
        bool isBox    = false;

        if (isPlayer) {
            half = PLAYER_SIZE * 0.5f;
            isBox = true;
        } else {
            orbIdx = std::rand() % 11;
            isBox  = (orbIdx == 10);
            half   = ORB_SIZE[orbIdx] * 0.5f;
        }

        b2BodyDef bd;
        bd.type = b2_dynamicBody;
        bd.angle = (std::rand() % 360) * (float)M_PI / 180.f;

        if (isPlayer) {
            bd.position.Set(
                (float)(std::rand() % std::max(1,(int)W)) / PPM,
                -(200.f + std::rand() % 800) / PPM
            );
        } else {
            bd.position.Set(
                (W * 0.1f + (std::rand() % std::max(1, (int)(W*0.8f)))) / PPM,
                -250.f / PPM
            );
        }

        auto* body = world->CreateBody(&bd);

        b2FixtureDef   fd;
        b2CircleShape  cs;
        b2PolygonShape ps;
        fd.density = 1.f; fd.restitution = 0.5f; fd.friction = isPlayer ? 0.7f : 1.f;

        if (isBox) { ps.SetAsBox(half/PPM, half/PPM); fd.shape = &ps; }
        else       { cs.m_radius = half/PPM;           fd.shape = &cs; }
        body->CreateFixture(&fd);

        if (!isPlayer)
            body->ApplyLinearImpulse({(10 - std::rand()%21) * 0.05f, 0.f}, body->GetWorldCenter(), true);

        CCNode* node = nullptr;
        if (isPlayer) {
            auto* gm = GameManager::get();
            auto* sp = SimplePlayer::create(gm->getPlayerFrame());
            if (sp) {
                sp->setColors(gm->colorForIdx(gm->getPlayerColor()), gm->colorForIdx(gm->getPlayerColor2()));
                if (gm->getPlayerGlow())
                    sp->setGlowOutline(gm->colorForIdx(gm->getPlayerGlowColor()));
                sp->setScale(1.f);
                sp->setPosition({-9999.f, -9999.f});
                this->addChild(sp, 3);
                node = sp;
            }
        } else {
            auto* obj = GameObject::createWithKey(ORB_IDS[orbIdx]);
            if (obj) {
                obj->setScale(1.f);
                obj->setPosition({-9999.f, -9999.f});
                this->addChild(obj, 2);
                node = obj;
            }
        }

        balls.push_back({body, node, half, orbIdx, isPlayer});
    }

    void startCycle() {
        auto* mod = Mod::get();
        numBalls  = (int)mod->getSettingValue<int64_t>("orb-count");
        cubePct   = (int)mod->getSettingValue<int64_t>("cube-chance");
        noFloor   = mod->getSettingValue<bool>("no-ground");
        spd       = (float)mod->getSettingValue<int64_t>("orb-speed") / 10.f;
        dropEvery = std::max(1, (int)(20.f / spd));
        if (numBalls < 1) numBalls = 1;

        tick      = 0;
        spawned   = 0;
        gotPlayer = false;
        filled    = false;
        draining  = false;
        accumulator = 0.f;
        drainWait = 5.f + (std::rand() % 1001) / 1000.f;

        for (auto& b : balls)
            if (b.node) b.node->removeFromParentAndCleanup(true);
        balls.clear();

        delete world; world = nullptr; ground = nullptr;

        world = new b2World({0.f, 9.8f * spd * 3.f});

        float W = scr.width, H = scr.height;
        makeWall(0,0, 0,H);
        makeWall(W,0, W,H);
        if (!noFloor)
            ground = makeWall(0,H, W,H);
    }

    bool init() override {
        auto* mod = Mod::get();
        bgCol = mod->getSettingValue<ccColor4B>("bg-color");
        bgMode = (mod->getSettingValue<bool>("bg-blur") && BlurAPI::isBlurAPIEnabled())
                    ? BgMode::Blur : BgMode::Color;

        if (!CCLayerColor::initWithColor({bgCol.r, bgCol.g, bgCol.b, 0})) return false;

        scr = CCDirector::get()->getWinSize();
        this->setContentSize(scr);
        this->setPosition(CCPointZero);

        if (bgMode == BgMode::Blur) BlurAPI::addBlur(this);

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
        startCycle();
        return true;
    }

    void update(float dt) override {
        if (dead) return;

        if (!fadeOk) {
            fadeT += dt;
            float t = std::min(fadeT / FADETIME, 1.f);
            if (t >= 1.f) fadeOk = true;
            GLubyte a = (bgMode == BgMode::Blur) ? 180 : bgCol.a;
            this->setOpacity((GLubyte)(a * t));
            return;
        }

        float W = scr.width, H = scr.height;
        tick++;

        while (spawned < numBalls && tick >= dropEvery * spawned) {
            spawnBall(W, H);
            spawned++;
        }

        if (!gotPlayer && spawned >= numBalls / 2) {
            gotPlayer = true;
            if ((std::rand() % 100) < cubePct)
                spawnBall(W, H, true);
        }

        if (!noFloor && !filled && spawned >= numBalls) {
            if (tick >= numBalls * dropEvery + (int)(drainWait * 60.f)) {
                filled   = true;
                draining = true;
                if (ground) { world->DestroyBody(ground); ground = nullptr; }
            }
        }

        if (!noFloor && draining) {
            bool allGone = true;
            for (auto& b : balls)
                if (b.body->GetPosition().y * PPM < H + 300.f) { allGone = false; break; }
            if (allGone) { startCycle(); return; }
        }

        if (noFloor && tick > numBalls * dropEvery + 500) { startCycle(); return; }

        static constexpr float STEP = 1.f/60.f;
        accumulator += dt;
        while (accumulator >= STEP) { world->Step(STEP, 8, 3); accumulator -= STEP; }

        for (auto& b : balls) {
            if (!b.node) continue;
            auto p = b.body->GetPosition();
            b.node->setPosition({p.x * PPM, H - p.y * PPM});
            b.node->setRotation(b.body->GetAngle() * (180.f / (float)M_PI));
        }
    }

public:
    static ScreensaverLayer* create() {
        auto* r = new ScreensaverLayer();
        if (r && r->init()) { r->autorelease(); r->setTag(2047); return r; }
        CC_SAFE_DELETE(r);
        return nullptr;
    }

    void dismiss() {
        if (dead) return;
        dead = true;
        this->stopAllActions();
        this->unscheduleAllSelectors();
        this->removeFromParentAndCleanup(true);
        g_active = false;
        g_idle   = 0.f;
    }

    bool ccTouchBegan(CCTouch*, CCEvent*) override { dismiss(); return true; }
    void keyDown(enumKeyCodes, double) override    { dismiss(); }
    void keyBackClicked() override                 { dismiss(); }
#ifdef GEODE_IS_DESKTOP
    void scrollWheel(float, float) override { dismiss(); }
#endif

    ~ScreensaverLayer() override {
        if (bgMode == BgMode::Blur) BlurAPI::removeBlur(this);
        delete world; world = nullptr;
    }
};

// scene hook - runs the idle counter
class $modify(MyScene, CCScene) {
    bool init() {
        if (!CCScene::init()) return false;
        this->schedule(schedule_selector(MyScene::tick), 0.f);
        return true;
    }

    void tick(float dt) {
        if (g_active || g_inLevel) { g_idle = 0.f; return; }
        g_idle += dt;

        float limit = (float)Mod::get()->getSettingValue<int64_t>("idle-timeout");
        if (g_idle < limit) return;

        g_idle   = 0.f;
        g_active = true;
        auto* ss = ScreensaverLayer::create();
        if (ss) { ss->setZOrder(9999); this->addChild(ss); }
        else g_active = false;
    }
};

class $modify(MyTouchDispatcher, CCTouchDispatcher) {
    void touches(CCSet* t, CCEvent* e, unsigned int type) {
        CCTouchDispatcher::touches(t, e, type);
        if (type == CCTOUCHBEGAN) g_idle = 0.f;
    }
};

// keyboard is annoying because it also needs to kill the screensaver, not just reset
class $modify(MyKbDispatcher, CCKeyboardDispatcher) {
    bool dispatchKeyboardMSG(enumKeyCodes key, bool down, bool repeat, double ts) {
        bool r = CCKeyboardDispatcher::dispatchKeyboardMSG(key, down, repeat, ts);
        if (down && !repeat) {
            g_idle = 0.f;
            auto* scene = CCDirector::get()->getRunningScene();
            if (scene) {
                if (auto* ss = scene->getChildByTag(2047))
                    static_cast<ScreensaverLayer*>(ss)->dismiss();
            }
        }
        return r;
    }
};

class $modify(MyPlayLayer, PlayLayer) {
    bool init(GJGameLevel* lvl, bool replay, bool noObjs) {
        g_inLevel = true;
        g_idle    = 0.f;
        auto* scene = CCDirector::get()->getRunningScene();
        if (scene) {
            if (auto* ss = scene->getChildByTag(2047))
                static_cast<ScreensaverLayer*>(ss)->dismiss();
        }
        return PlayLayer::init(lvl, replay, noObjs);
    }

    void onQuit() {
        g_inLevel = false;
        g_idle    = 0.f;
        PlayLayer::onQuit();
    }
};
