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

static void resetIdle();
static void dismissActive();

static bool  s_inPlayLayer = false;
static float s_idleTimer   = 0.f;
static bool  s_ssActive    = false;

static constexpr float PPM           = 40.f;
static constexpr float FADE_DURATION = 0.5f;

// TO FIX TOMORROW
// 36,   84,   141,   1022,  1330,  1333,  1704,  1751,  3004,  3027,  1594
// yellow, blue, pink, green, black, red, dash, dash2, spider, teleport, toggle
static const int   ORB_IDS[11]      = { 36,   84,   141,   1022,  1330,  1333,  1704,  1751,  3004,  3027,  1594  };
// full content sizes from device log — used directly as box2d radius or half-extent
static const float ORB_FULL_SIZE[11] = { 32.3f, 32.5f, 32.6f, 31.76f, 35.f, 33.57f, 41.f,  41.f,  41.f,  39.1f, 30.5f };
// SimplePlayer content size on device
static constexpr float PLAYER_FULL_SIZE = 54.f;

enum class BgMode { Color, Blur };

class ScreensaverLayer : public CCLayerColor {
public:
    static ScreensaverLayer* create() {
        auto* ret = new ScreensaverLayer();
        if (ret && ret->init()) { ret->autorelease(); return ret; }
        CC_SAFE_DELETE(ret);
        return nullptr;
    }

    void dismiss() {
        if (m_dismissed) return;
        m_dismissed = true;
        this->stopAllActions();
        this->unscheduleAllSelectors();
        this->removeFromParentAndCleanup(true);
        s_ssActive  = false;
        s_idleTimer = 0.f;
    }

private:

    struct Ball {
        b2Body* body     = nullptr;
        float   radius   = 0.f;
        int     orbIdx   = 0;
        bool    isPlayer = false;
        CCNode* node     = nullptr;
    };

    b2World*          m_world      = nullptr;
    b2Body*           m_wallBot    = nullptr;
    std::vector<Ball> m_balls;

    int   m_globalTime    = 0;
    int   m_nextSpawn     = 0;
    bool  m_playerSpawned = false;
    bool  m_fillingDone   = false;
    bool  m_draining      = false;
    bool  m_dismissed     = false;

    float m_physAccum  = 0.f;
    float m_drainDelay = 5.f;

    int     m_numBalls   = 120;
    float   m_speedMult  = 1.f;
    int     m_cubeChance = 50;
    bool    m_noGround   = false;
    int     m_dropTime   = 2;
    BgMode  m_bgMode     = BgMode::Color;
    ccColor4B m_bgColor  = {0, 0, 0, 255};

    bool  m_fadeDone   = false;
    float m_fadeTimer  = 0.f;

    CCSize m_screen;

    bool init() override {
        auto* mod = Mod::get();
        m_bgColor = mod->getSettingValue<ccColor4B>("bg-color");

        bool wantBlur = mod->getSettingValue<bool>("bg-blur");
        m_bgMode = (wantBlur && BlurAPI::isBlurAPIEnabled()) ? BgMode::Blur : BgMode::Color;

        ccColor4B startColor = {m_bgColor.r, m_bgColor.g, m_bgColor.b, 0};
        if (!CCLayerColor::initWithColor(startColor))
            return false;

        m_screen = CCDirector::get()->getWinSize();
        this->setContentSize(m_screen);
        this->setPosition(CCPointZero);

        if (m_bgMode == BgMode::Blur)
            BlurAPI::addBlur(this);

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

    bool ccTouchBegan(CCTouch*, CCEvent*) override { dismiss(); return true; }
    void keyDown(enumKeyCodes, double) override    { dismiss(); }
    void keyBackClicked() override                 { dismiss(); }
#ifdef GEODE_IS_DESKTOP
    void scrollWheel(float, float) override        { dismiss(); }
#endif

    void startCycle() {
        auto* mod      = Mod::get();
        m_numBalls     = (int)mod->getSettingValue<int64_t>("orb-count");
        m_cubeChance   = (int)mod->getSettingValue<int64_t>("cube-chance");
        m_noGround     = mod->getSettingValue<bool>("no-ground");
        float rawSpeed = (float)mod->getSettingValue<int64_t>("orb-speed");
        m_speedMult    = rawSpeed / 10.f;
        m_dropTime     = std::max(1, (int)(20.f / m_speedMult));

        if (m_numBalls < 1) m_numBalls = 1;

        m_globalTime    = 0;
        m_nextSpawn     = 0;
        m_playerSpawned = false;
        m_fillingDone   = false;
        m_draining      = false;
        m_physAccum     = 0.f;
        m_drainDelay    = 5.f + (std::rand() % 1001) / 1000.f;

        for (auto& b : m_balls)
            if (b.node) b.node->removeFromParentAndCleanup(true);
        m_balls.clear();

        delete m_world;
        m_world   = nullptr;
        m_wallBot = nullptr;

        b2Vec2 grav(0.f, 9.8f * m_speedMult * 3.f);
        m_world = new b2World(grav);

        float W = m_screen.width;
        float H = m_screen.height;

        auto makeWall = [&](float x1, float y1, float x2, float y2) -> b2Body* {
            b2BodyDef bd; bd.type = b2_staticBody;
            b2Body* body = m_world->CreateBody(&bd);
            b2EdgeShape es;
            es.SetTwoSided(b2Vec2(x1/PPM, y1/PPM), b2Vec2(x2/PPM, y2/PPM));
            b2FixtureDef fd; fd.shape = &es; fd.restitution = 0.5f; fd.friction = 0.7f;
            body->CreateFixture(&fd);
            return body;
        };

        makeWall(0, 0, 0, H);
        makeWall(W, 0, W, H);
        if (!m_noGround)
            m_wallBot = makeWall(0, H, W, H);
    }

    void update(float dt) override {
        if (m_dismissed) return;

        if (!m_fadeDone) {
            m_fadeTimer += dt;
            float t = m_fadeTimer / FADE_DURATION;
            if (t >= 1.f) { t = 1.f; m_fadeDone = true; }

            GLubyte targetAlpha = (m_bgMode == BgMode::Blur) ? 180 : m_bgColor.a;
            this->setOpacity((GLubyte)(targetAlpha * t));
            return;
        }

        float W = m_screen.width;
        float H = m_screen.height;
        m_globalTime++;

        while (m_nextSpawn < m_numBalls &&
               m_globalTime >= m_dropTime * m_nextSpawn)
        {
            spawnOrb(W, H);
            m_nextSpawn++;
        }

        if (!m_playerSpawned && m_nextSpawn >= m_numBalls / 2) {
            m_playerSpawned = true;
            if ((std::rand() % 100) < m_cubeChance)
                spawnPlayerCube(W, H);
        }

        if (!m_noGround && !m_fillingDone && m_nextSpawn >= m_numBalls) {
            int drainFrame = m_numBalls * m_dropTime + (int)(m_drainDelay * 60.f);
            if (m_globalTime >= drainFrame) {
                m_fillingDone = true;
                m_draining    = true;
                if (m_wallBot) { m_world->DestroyBody(m_wallBot); m_wallBot = nullptr; }
            }
        }

        if (!m_noGround && m_draining) {
            bool allOff = true;
            for (auto& b : m_balls)
                if (b.body->GetPosition().y * PPM < H + 300.f) { allOff = false; break; }
            if (allOff) { startCycle(); return; }
        }
        if (m_noGround && m_globalTime > m_numBalls * m_dropTime + 500) {
            startCycle(); return;
        }

        static constexpr float FIXED_STEP = 1.f / 60.f;

        m_physAccum += dt;
        while (m_physAccum >= FIXED_STEP) {
            m_world->Step(FIXED_STEP, 8, 3);
            m_physAccum -= FIXED_STEP;
        }

        for (auto& b : m_balls) {
            if (!b.node) continue;
            float px = b.body->GetPosition().x * PPM;
            float py = b.body->GetPosition().y * PPM;
            b.node->setPosition({px, H - py});
            float angDeg = -b.body->GetAngle() * (180.f / (float)M_PI);
            b.node->setRotation(angDeg);
        }
    }

    void spawnOrb(float W, float) {
        int   orbIdx  = std::rand() % 11;
        bool  isBox   = (orbIdx == 10);
        float size    = ORB_FULL_SIZE[orbIdx]; // full content size from device
        float half    = size * 0.5f;

        b2BodyDef bd;
        bd.type = b2_dynamicBody;
        bd.position.Set(
            (W * 0.1f + (std::rand() % std::max(1, (int)(W * 0.8f)))) / PPM,
            -250.f / PPM
        );
        bd.angle = (float)(std::rand() % 360) * ((float)M_PI / 180.f);

        b2Body* body = m_world->CreateBody(&bd);

        b2FixtureDef   fd;
        b2CircleShape  cs;
        b2PolygonShape ps;
        fd.density = 1.f; fd.restitution = 0.5f; fd.friction = 1.f;

        if (isBox) {
            ps.SetAsBox(half / PPM, half / PPM);
            fd.shape = &ps;
        } else {
            cs.m_radius = half / PPM;
            fd.shape    = &cs;
        }
        body->CreateFixture(&fd);
        body->ApplyLinearImpulse(
            b2Vec2((10 - std::rand() % 21) * 0.05f, 0.f),
            body->GetWorldCenter(), true
        );

        auto* obj = GameObject::createWithKey(ORB_IDS[orbIdx]);
        if (obj) {
            this->addChild(obj, 2);
            obj->setScale(1.f);
            obj->setPosition({-9999.f, -9999.f});
        }

        m_balls.push_back({body, half, orbIdx, false, obj});
    }

    void spawnPlayerCube(float W, float) {
        auto* gm = GameManager::get();
        float half = PLAYER_FULL_SIZE * 0.5f;

        b2BodyDef bd;
        bd.type = b2_dynamicBody;
        bd.position.Set(
            ((float)(std::rand() % std::max(1, (int)W))) / PPM,
            -(200.f + std::rand() % 800) / PPM
        );
        bd.angle = (float)(std::rand() % 360) * ((float)M_PI / 180.f);

        b2Body* body = m_world->CreateBody(&bd);
        b2PolygonShape ps;
        ps.SetAsBox(half / PPM, half / PPM);
        b2FixtureDef fd;
        fd.shape = &ps; fd.density = 1.f; fd.restitution = 0.5f; fd.friction = 0.7f;
        body->CreateFixture(&fd);

        auto* sp = SimplePlayer::create(gm->getPlayerFrame());
        if (sp) {
            sp->setColors(
                gm->colorForIdx(gm->getPlayerColor()),
                gm->colorForIdx(gm->getPlayerColor2())
            );
            if (gm->getPlayerGlow())
                sp->setGlowOutline(gm->colorForIdx(gm->getPlayerGlowColor()));

            this->addChild(sp, 3);
            sp->setScale(1.f);
            sp->setPosition({-9999.f, -9999.f});
        }

        m_balls.push_back({body, half, 0, true, sp});
    }

    ~ScreensaverLayer() override {
        if (m_bgMode == BgMode::Blur)
            BlurAPI::removeBlur(this);
        delete m_world;
        m_world = nullptr;
    }
};

static void resetIdle() {
    s_idleTimer = 0.f;
}

static void dismissActive() {
    auto* scene = CCDirector::get()->getRunningScene();
    if (!scene) return;
    auto* children = scene->getChildren();
    if (!children) return;
    for (unsigned i = 0; i < children->count(); i++) {
        if (auto* ss = dynamic_cast<ScreensaverLayer*>(children->objectAtIndex(i))) {
            ss->dismiss();
            return;
        }
    }
}

class $modify(OSSCCScene, CCScene) {
    bool init() {
        if (!CCScene::init()) return false;
        this->schedule(schedule_selector(OSSCCScene::idleTick), 0.f);
        return true;
    }

    void idleTick(float dt) {
        if (s_ssActive || s_inPlayLayer) {
            s_idleTimer = 0.f;
            return;
        }

        s_idleTimer += dt;

        float timeout = (float)Mod::get()->getSettingValue<int64_t>("idle-timeout");
        if (s_idleTimer < timeout) return;

        s_idleTimer = 0.f;
        s_ssActive  = true;

        auto* ss = ScreensaverLayer::create();
        if (ss) {
            ss->setZOrder(9999);
            this->addChild(ss);
        } else {
            s_ssActive = false;
        }
    }
};

class $modify(OSSCCTouchDispatcher, CCTouchDispatcher) {
    void touches(CCSet* touches, CCEvent* event, unsigned int type) {
        CCTouchDispatcher::touches(touches, event, type);
        if (type == CCTOUCHBEGAN) resetIdle();
    }
};

class $modify(OSSCCKeyboardDispatcher, CCKeyboardDispatcher) {
    bool dispatchKeyboardMSG(enumKeyCodes key, bool isKeyDown,
                             bool isKeyRepeat, double timestamp)
    {
        bool ret = CCKeyboardDispatcher::dispatchKeyboardMSG(
            key, isKeyDown, isKeyRepeat, timestamp);
        if (isKeyDown && !isKeyRepeat) {
            resetIdle();
            dismissActive();
        }
        return ret;
    }
};

// fuck
class $modify(OSSPlayLayer, PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        s_inPlayLayer = true;
        s_idleTimer   = 0.f;
        dismissActive();
        return PlayLayer::init(level, useReplay, dontCreateObjects);
    }

    void onQuit() {
        s_inPlayLayer = false;
        s_idleTimer   = 0.f;
        PlayLayer::onQuit();
    }
};
