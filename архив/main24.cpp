#include "raylib.h"
#include <fstream>
#include <cmath>
#include <sstream>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------
float noteSpeed = 0.75f;

enum class MenuTab { PLAY, CONTROLS, ABOUT, SETTINGS, EXIT };   // pixels per millisecond
constexpr float HIT_Y = 900.0f;  // y-coordinate of the hit line
constexpr float NOTE_RADIUS = 27.0f;
constexpr float LANE_WIDTH = 144.0f;

// Lane centers (3 vertical lanes, centered on screen)
const float LANE_CENTERS[3] = { 720.0f, 960.0f, 1200.0f };
Color LANE_COLORS[3] = {
    { 232,  68,  68, 255 },
    {  70, 200, 110, 255 },
    {  60, 160, 245, 255 }
};

// ---------------------------------------------------------------------------
// Note
// ---------------------------------------------------------------------------
enum class NoteType { NORMAL, TRAP, HOLD };
enum class HoldState { IDLE, HOLDING, COMPLETED, FAILED };

struct Note
{
    int      time_ms;
    int      lane;          // 1-based
    NoteType type;
    int      duration_ms;   // only relevant for HOLD
    bool      active      = true;
    bool      scrollingOff = false;          // keep rendering after miss until off screen
    HoldState holdState    = HoldState::IDLE;
};

// ---------------------------------------------------------------------------
// Judgment
// ---------------------------------------------------------------------------
enum class GameState { MENU, GAMEPLAY, PAUSED, RESULT };
enum class Judgment { NONE, PERFECT, GOOD, MISS, TRAP, HOLD_OK, HOLD_MISS };

// ---------------------------------------------------------------------------
// Level definitions
// ---------------------------------------------------------------------------
struct LevelDef { const char* name; const char* musicPath; const char* chartPath; const char* difficulty; Color diffColor; };
constexpr int LEVEL_COUNT = 4;
const LevelDef LEVEL_DEFS[LEVEL_COUNT] = {
    { "Уровень 1", "assets/track.ogg",  "assets/example.json",  "Легкий",  {  80, 220, 100, 255 } },
    { "Уровень 2", "assets/track2.ogg", "assets/example2.json", "Средний", { 255, 200,  40, 255 } },
    { "Уровень 3", "assets/track3.ogg", "assets/example3.json", "Сложный", { 230,  70,  70, 255 } },
    { "Уровень 4", "assets/track4.ogg", "assets/example4.json", "Безумие", { 200,  40, 255, 255 } },
};

int highScores[LEVEL_COUNT] = { 0 };

void loadHighScores() {
    std::ifstream f("assets/scores.txt");
    if (f.is_open()) {
        for (int i = 0; i < LEVEL_COUNT; i++) {
            f >> highScores[i];
        }
    }
}

void saveHighScores() {
    std::ofstream f("assets/scores.txt");
    if (f.is_open()) {
        for (int i = 0; i < LEVEL_COUNT; i++) {
            f << highScores[i] << " ";
        }
    }
}

// Hit windows calibrated to noteSpeed=0.5 px/ms and NOTE_RADIUS=18px:
//   PERFECT 45ms → note 33.75px from line → bottom edge 6.75px above (visually touching)
//   GOOD    90ms → note 67.5px from line  → bottom edge 40.5px above  (clearly approaching)
constexpr int HIT_WINDOW_PERFECT = 45;   // ±45 ms
constexpr int HIT_WINDOW_GOOD = 90;   // ±90 ms
constexpr int HIT_WINDOW_MISS = 200;  // ±200 ms — auto-miss / ghost-tap boundary

// ---------------------------------------------------------------------------
// Parse chart  (no external libraries — plain std::string search)
// ---------------------------------------------------------------------------

struct ChartData {
    int bpm = 0;
    std::string difficulty = "";
    Color bgColor = { 12, 12, 22, 255 };
    Color laneColors[3] = {
        { 232,  68,  68, 255 },
        {  70, 200, 110, 255 },
        {  60, 160, 245, 255 }
    };
    bool glitchVhs = false;
    bool tileGlow = false;
    std::vector<Note> notes;
};

Color ColorFromHex(const std::string& hex) {
    Color color = { 0, 0, 0, 255 };
    if (hex.empty()) return color;
    size_t offset = (hex[0] == '#') ? 1 : 0;
    if (hex.length() - offset >= 6) {
        try {
            unsigned int val = std::stoul(hex.substr(offset, 6), nullptr, 16);
            color.r = (val >> 16) & 0xFF;
            color.g = (val >> 8) & 0xFF;
            color.b = val & 0xFF;
        } catch (...) {
            // Ignore invalid hex
        }
    }
    return color;
}

static bool jsonBool(const std::string& src, const std::string& key, bool def = false)
{
    std::string needle = "\"" + key + "\"";
    size_t p = src.find(needle);
    if (p == std::string::npos) return def;
    p = src.find(':', p + needle.size());
    if (p == std::string::npos) return def;
    p = src.find_first_not_of(" \t\r\n", p + 1);
    if (p == std::string::npos) return def;
    if (src.compare(p, 4, "true") == 0) return true;
    if (src.compare(p, 5, "false") == 0) return false;
    return def;
}

// Extract the integer value for a JSON key inside a string block
static int jsonInt(const std::string& src, const std::string& key, int def = 0)
{
    std::string needle = "\"" + key + "\"";
    size_t p = src.find(needle);
    if (p == std::string::npos) return def;
    p = src.find(':', p + needle.size());
    if (p == std::string::npos) return def;
    p = src.find_first_of("-0123456789", p + 1);
    if (p == std::string::npos) return def;
    return std::stoi(src.substr(p));
}

// Extract the string value for a JSON key inside a string block
static std::string jsonStr(const std::string& src, const std::string& key)
{
    std::string needle = "\"" + key + "\"";
    size_t p = src.find(needle);
    if (p == std::string::npos) return {};
    p = src.find(':', p + needle.size());
    if (p == std::string::npos) return {};
    p = src.find('"', p + 1);
    if (p == std::string::npos) return {};
    size_t end = src.find('"', p + 1);
    if (end == std::string::npos) return {};
    return src.substr(p + 1, end - p - 1);
}

bool parseChart(const std::string& path, ChartData& outData)
{
    std::ifstream f(path);
    if (!f.is_open()) return false;

    std::ostringstream ss;
    ss << f.rdbuf();
    const std::string src = ss.str();
    if (src.empty()) return false;

    outData.bpm = jsonInt(src, "bpm");
    
    std::string diff = jsonStr(src, "difficulty");
    if (!diff.empty()) outData.difficulty = diff;
    
    std::string bgCol = jsonStr(src, "bg_color");
    if (!bgCol.empty()) outData.bgColor = ColorFromHex(bgCol);
    
    std::string lane1 = jsonStr(src, "lane_1");
    if (!lane1.empty()) outData.laneColors[0] = ColorFromHex(lane1);
    
    std::string lane2 = jsonStr(src, "lane_2");
    if (!lane2.empty()) outData.laneColors[1] = ColorFromHex(lane2);
    
    std::string lane3 = jsonStr(src, "lane_3");
    if (!lane3.empty()) outData.laneColors[2] = ColorFromHex(lane3);
    
    outData.glitchVhs = jsonBool(src, "glitch_vhs");
    outData.tileGlow = jsonBool(src, "tile_glow");

    // Find the "notes" array and iterate over each { } object
    size_t arrStart = src.find("\"notes\"");
    if (arrStart == std::string::npos) return false;
    arrStart = src.find('[', arrStart);
    if (arrStart == std::string::npos) return false;

    size_t pos = arrStart + 1;
    while (true)
    {
        size_t objStart = src.find('{', pos);
        size_t objEnd = src.find('}', pos);
        if (objStart == std::string::npos || objEnd == std::string::npos) break;
        if (objStart > objEnd) break;

        std::string obj = src.substr(objStart + 1, objEnd - objStart - 1);

        Note note{};
        note.time_ms = jsonInt(obj, "time_ms");
        note.lane = jsonInt(obj, "lane");
        note.duration_ms = jsonInt(obj, "duration_ms", 0);

        std::string t = jsonStr(obj, "type");
        if (t == "trap") note.type = NoteType::TRAP;
        else if (t == "hold") note.type = NoteType::HOLD;
        else                  note.type = NoteType::NORMAL;

        outData.notes.push_back(note);
        pos = objEnd + 1;
    }

    return outData.bpm > 0 && !outData.notes.empty();
}

// ---------------------------------------------------------------------------
// BackgroundManager — audio-reactive animated star field
// ---------------------------------------------------------------------------
class BackgroundManager
{
public:
    static constexpr int STAR_COUNT = 80;

    struct Star
    {
        float x, y;           // position
        float baseSize;       // base radius (2–6 px)
        float speed;          // vertical drift speed (px/s)
        float phase;          // unique phase offset for pulsing
        int   points;         // 4 or 5 (star shape)
    };

    Star stars[STAR_COUNT]{};
    float smoothVolume = 0.0f;   // lerped volume
    float time         = 0.0f;   // accumulated time (seconds)
    int   screenW, screenH;

    void Init(int w, int h)
    {
        screenW = w;
        screenH = h;
        for (int i = 0; i < STAR_COUNT; i++)
        {
            stars[i].x        = (float)GetRandomValue(0, w);
            stars[i].y        = (float)GetRandomValue(0, h);
            stars[i].baseSize = 2.0f + (float)GetRandomValue(0, 40) * 0.1f; // 2–6
            stars[i].speed    = 8.0f + (float)GetRandomValue(0, 22);        // 8–30 px/s
            stars[i].phase    = (float)GetRandomValue(0, 6283) * 0.001f;    // 0–2π
            stars[i].points   = GetRandomValue(0, 1) ? 4 : 5;
        }
    }

    void Update(float dt, float musicVolume)
    {
        // Smooth interpolation — no sudden jumps
        smoothVolume += (musicVolume - smoothVolume) * dt * 4.0f;
        time += dt;

        for (int i = 0; i < STAR_COUNT; i++)
        {
            stars[i].y += stars[i].speed * dt;
            // subtle horizontal sway
            stars[i].x += sinf(time * 0.5f + stars[i].phase) * 6.0f * dt;
            // wrap around
            if (stars[i].y > (float)screenH + 20.0f)
            { stars[i].y = -20.0f; stars[i].x = (float)GetRandomValue(0, screenW); }
            if (stars[i].x < -20.0f)  stars[i].x += (float)screenW + 40.0f;
            if (stars[i].x > (float)screenW + 20.0f) stars[i].x -= (float)screenW + 40.0f;
        }
    }

    void Draw() const
    {
        for (int i = 0; i < STAR_COUNT; i++)
        {
            const Star& s = stars[i];
            // Per-star pulse using sin + unique phase
            float pulse = 0.5f + 0.5f * sinf(time * 2.0f + s.phase);
            // Scale and brightness react to smoothVolume
            float scale  = 1.0f + smoothVolume * 0.8f + pulse * smoothVolume * 0.5f;
            float radius = s.baseSize * scale;
            unsigned char alpha = (unsigned char)(40.0f + smoothVolume * 140.0f + pulse * 40.0f);
            if (alpha > 220) alpha = 220;

            // Draw a simple star shape (rotated lines through center)
            Color col = { 180, 200, 255, alpha };
            int pts = s.points;
            float angleStep = 3.14159265f / (float)pts;
            for (int p = 0; p < pts; p++)
            {
                float ang = angleStep * (float)p + time * 0.3f + s.phase;
                float dx = cosf(ang) * radius;
                float dy = sinf(ang) * radius;
                DrawLineEx({ s.x - dx, s.y - dy }, { s.x + dx, s.y + dy },
                    1.2f + smoothVolume * 0.8f, col);
            }
            // Tiny bright core
            if (radius > 3.0f)
                DrawCircle((int)s.x, (int)s.y, 1.0f + smoothVolume, { 220, 230, 255, alpha });
        }
    }
};

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

// Helper for combo multiplier
int getComboMultiplier(int combo)
{
    int mult = 1 + (combo / 10);
    if (mult > 5) mult = 5;
    return mult;
}

int main(void)
{
    const int SCREEN_W = 1920;
    const int SCREEN_H = 1080;

    SetConfigFlags(FLAG_FULLSCREEN_MODE);
    InitWindow(SCREEN_W, SCREEN_H, "Синхрон");
    InitAudioDevice();
    SetTargetFPS(GetMonitorRefreshRate(GetCurrentMonitor()));
    SetExitKey(KEY_NULL);

    RenderTexture2D canvas = LoadRenderTexture(SCREEN_W, SCREEN_H);
    SetTextureFilter(canvas.texture, TEXTURE_FILTER_BILINEAR);
    SetTextureWrap(canvas.texture, TEXTURE_WRAP_CLAMP);

    // --- Load assets ---
    
    loadHighScores();

    // Font with ASCII + full Cyrillic block
    const int CP_COUNT = 96 + 256 + 1;
    int codepoints[CP_COUNT];
    for (int i = 0; i < 96; i++) codepoints[i] = 32 + i;
    for (int i = 0; i < 256; i++) codepoints[96 + i] = 0x0400 + i;
    codepoints[96 + 256] = 0x2014;  // em dash —
    Font font = LoadFontEx("assets/font.ttf", 96, codepoints, CP_COUNT);
    SetTextureFilter(font.texture, TEXTURE_FILTER_BILINEAR);

    bool fontOk = (font.texture.id > 0);

    // --- Pre-parse all level charts ---
    std::vector<ChartData> levelTemplates(LEVEL_COUNT);
    bool levelChartOk[LEVEL_COUNT] = {};
    {
      for (int i = 0; i < LEVEL_COUNT; i++)
          levelChartOk[i] = parseChart(LEVEL_DEFS[i].chartPath, levelTemplates[i]);
    }

    // Load music for level 0 initially
    int               currentLevel = 0;
    Music             music        = LoadMusicStream(LEVEL_DEFS[0].musicPath);
    bool              musicOk      = (music.frameCount > 0);
    std::vector<Note> notes        = levelTemplates[0].notes;
    bool              chartOk      = levelChartOk[0];
    Color             currentBgColor = levelTemplates[0].bgColor;
    bool              currentTileGlow = levelTemplates[0].tileGlow;
    bool              currentGlitchVhs = levelTemplates[0].glitchVhs;
    for(int i=0; i<3; ++i) LANE_COLORS[i] = levelTemplates[0].laneColors[i];

    // --- Game state ---
    GameState state = GameState::MENU;
    MenuTab  currentTab = MenuTab::PLAY;
    bool     insideTab = false;
    bool     exitModal = false;
    int      exitSelection = 1; // 0=ДА, 1=НЕТ
    int      settingsSelection = 0; // 0=Громкость, 1=Скорость, 2=Полный экран
    float    globalVolume = 1.0f;
    bool     isFullscreen = true;

    int      score = 0;
    int      perfectCount = 0;
    int      goodCount = 0;
    int      missCount = 0;
    int      trapErrors = 0;
    int      holdOkCount = 0;
    int      holdMissCount = 0;
    int      combo = 0;
    int      maxCombo = 0;
    Judgment lastJudgment = Judgment::NONE;
    float    judgmentTimer = 0.0f;
    int      pauseSelection = 0;
    bool     justPaused = false;
    bool     pauseExitModal = false;
    int      pauseExitSelection = 1; // 0=ДА, 1=НЕТ
    int      selectedLevel = 0;
    float    flashAlpha = 0.0f;
    float    shakeTime = 0.0f;
    int      shakeOffsetX = 0;
    int      shakeOffsetY = 0;
    float    hitEffect[3]        = { 0.0f, 0.0f, 0.0f };
    bool     hitEffectPerfect[3] = { false, false, false };
    float    laneGlow[3]         = { 0.0f, 0.0f, 0.0f };
    float    comboPopTimer = 0.0f;
    float    scorePopTimer = 0.0f;
    bool     comboPopPerf  = false;
    bool     scorePopPerf  = false;
    bool     fadingOut      = false;
    float    fadeOutTimer   = 0.0f;
    const float FADE_OUT_DURATION = 2.5f;
    float    swayTime       = 0.0f;
    float    hudCurrentTimeMs = 0.0f;
    float    visualTimeMs = 0.0f;
    float    smoothCA        = 0.0f;

    BackgroundManager bgManager;
    bgManager.Init(SCREEN_W, SCREEN_H);

    // Helper: draw text with loaded font, fallback to default raylib font
    auto drawTxt = [&](const char* text, int x, int y, int size, Color col)
        {
            if (fontOk)
                DrawTextEx(font, text, { (float)x, (float)y }, (float)size, 1.0f, col);
            else
                DrawText(text, x, y, size, col);
        };

    // Reset all game data and (re)start the track
    auto resetGame = [&](int levelIdx)
        {
            if (levelIdx != currentLevel)
            {
                if (musicOk) UnloadMusicStream(music);
                music        = LoadMusicStream(LEVEL_DEFS[levelIdx].musicPath);
                musicOk      = (music.frameCount > 0);
                chartOk      = levelChartOk[levelIdx];
                currentLevel = levelIdx;
            }
            notes = levelTemplates[levelIdx].notes;
            currentBgColor = levelTemplates[levelIdx].bgColor;
            currentTileGlow = levelTemplates[levelIdx].tileGlow;
            currentGlitchVhs = levelTemplates[levelIdx].glitchVhs;
            for(int i=0; i<3; ++i) LANE_COLORS[i] = levelTemplates[levelIdx].laneColors[i];
            
            score = perfectCount = goodCount = missCount = 0;
            trapErrors = holdOkCount = holdMissCount = combo = maxCombo = 0;
            lastJudgment = Judgment::NONE;
            judgmentTimer = 0.0f;
            fadingOut = false; fadeOutTimer = 0.0f; visualTimeMs = 0.0f;
            if (musicOk) { StopMusicStream(music); PlayMusicStream(music); }
            state = GameState::GAMEPLAY;
        };

    bool running = true;
    while (!WindowShouldClose() && running)
    {
        float dt = GetFrameTime();
        swayTime += dt;

        // CA pulse: interference of two sin waves → irregular music-like peaks
        if (currentLevel == 2 && (state == GameState::GAMEPLAY || state == GameState::PAUSED))
        {
            float b1 = sinf(swayTime * 5.5f);
            float b2 = sinf(swayTime * 3.2f);
            float caTarget = b1 * b2;
            caTarget = caTarget > 0.0f ? caTarget * caTarget * caTarget : 0.0f; // cubed: sharp peaks
            float speed = caTarget > smoothCA ? 14.0f : 3.5f; // fast attack, slow decay
            smoothCA += (caTarget - smoothCA) * dt * speed;
        }
        else
        {
            smoothCA = 0.0f;
        }

        BeginTextureMode(canvas);
        ClearBackground(currentBgColor);

        // ================================================================
        // MENU
        // ================================================================
        if (state == GameState::MENU)
        {
            bgManager.Update(dt, 0.5f);
            bgManager.Draw();

            auto cTxt = [&](const char* t, float y, int sz, Color c) {
                Vector2 s = MeasureTextEx(font, t, (float)sz, 1.0f);
                DrawTextEx(font, t, { SCREEN_W / 2.0f - s.x / 2, y }, (float)sz, 1.0f, c);
            };

            Vector2 tSz = MeasureTextEx(font, "СИНХРОН", 132.0f, 2.0f);
            DrawTextEx(font, "СИНХРОН", { SCREEN_W / 2.0f - tSz.x / 2, 120 }, 132.0f, 2.0f, WHITE);
            DrawLineEx({ SCREEN_W / 2.0f - 345, 280 }, { SCREEN_W / 2.0f + 345, 280 }, 3.0f, { 80, 80, 130, 200 });

            if (exitModal)
            {
                // Modal input
                if (IsKeyPressed(KEY_LEFT) || IsKeyPressed(KEY_A)) exitSelection = 0; // ДА
                if (IsKeyPressed(KEY_RIGHT) || IsKeyPressed(KEY_D)) exitSelection = 1; // НЕТ
                if (IsKeyPressed(KEY_ENTER)) {
                    if (exitSelection == 0) running = false;
                    else exitModal = false;
                }
                if (IsKeyPressed(KEY_ESCAPE)) exitModal = false;

                // Draw Modal
                DrawRectangle(0, 0, SCREEN_W, SCREEN_H, { 0, 0, 0, 150 });
                int pw = 700, ph = 350;
                int px = SCREEN_W / 2 - pw / 2, py = SCREEN_H / 2 - ph / 2;
                DrawRectangle(px, py, pw, ph, { 20, 22, 35, 240 });
                DrawRectangleLines(px, py, pw, ph, { 80, 80, 140, 200 });

                cTxt("Вы действительно хотите выйти?", (float)py + 80, 48, WHITE);

                const char* btnYes = "ДА";
                const char* btnNo = "НЕТ";
                
                Vector2 szYes = MeasureTextEx(font, btnYes, 48.0f, 1.0f);
                Vector2 szNo = MeasureTextEx(font, btnNo, 48.0f, 1.0f);
                
                Rectangle recYes = { (float)px + 150, (float)py + 200, szYes.x + 60, 70 };
                Rectangle recNo = { (float)px + pw - 150 - szNo.x - 60, (float)py + 200, szNo.x + 60, 70 };

                Vector2 mouse = GetMousePosition();
                if (CheckCollisionPointRec(mouse, recYes)) {
                    exitSelection = 0;
                    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) running = false;
                }
                if (CheckCollisionPointRec(mouse, recNo)) {
                    exitSelection = 1;
                    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) exitModal = false;
                }

                DrawRectangleRec(recYes, exitSelection == 0 ? Color{255,255,255,40} : BLANK);
                DrawRectangleLinesEx(recYes, 2.0f, exitSelection == 0 ? WHITE : GRAY);
                DrawTextEx(font, btnYes, { recYes.x + 30, recYes.y + 11 }, 48.0f, 1.0f, exitSelection == 0 ? WHITE : GRAY);

                DrawRectangleRec(recNo, exitSelection == 1 ? Color{255,255,255,40} : BLANK);
                DrawRectangleLinesEx(recNo, 2.0f, exitSelection == 1 ? WHITE : GRAY);
                DrawTextEx(font, btnNo, { recNo.x + 30, recNo.y + 11 }, 48.0f, 1.0f, exitSelection == 1 ? WHITE : GRAY);
            }
            else if (!insideTab)
            {
                // Main Menu Tabs
                const char* tabNames[] = { "ИГРАТЬ", "УПРАВЛЕНИЕ", "ОБ АВТОРЕ", "НАСТРОЙКИ", "ВЫХОД" };
                int tabCount = 5;
                float startY = 350.0f;
                float spacing = 90.0f;

                if (IsKeyPressed(KEY_UP) || IsKeyPressed(KEY_W)) {
                    currentTab = (MenuTab)(((int)currentTab + tabCount - 1) % tabCount);
                }
                if (IsKeyPressed(KEY_DOWN) || IsKeyPressed(KEY_S)) {
                    currentTab = (MenuTab)(((int)currentTab + 1) % tabCount);
                }
                if (IsKeyPressed(KEY_ENTER)) {
                    if (currentTab == MenuTab::EXIT) { exitModal = true; exitSelection = 1; }
                    else insideTab = true;
                }

                Vector2 mouse = GetMousePosition();

                for (int i = 0; i < tabCount; i++) {
                    Vector2 sz = MeasureTextEx(font, tabNames[i], 54.0f, 1.0f);
                    Rectangle btnRec = { SCREEN_W / 2.0f - sz.x / 2.0f - 40, startY + i * spacing - 10, sz.x + 80, 74 };
                    
                    if (CheckCollisionPointRec(mouse, btnRec)) {
                        currentTab = (MenuTab)i;
                        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                            if (currentTab == MenuTab::EXIT) { exitModal = true; exitSelection = 1; }
                            else insideTab = true;
                        }
                    }

                    Color c = (currentTab == (MenuTab)i) ? WHITE : Color{ 160, 160, 220, 200 };
                    if (currentTab == (MenuTab)i) {
                        DrawTextEx(font, ">", { btnRec.x, btnRec.y + 10 }, 54.0f, 1.0f, WHITE);
                        DrawTextEx(font, "<", { btnRec.x + btnRec.width - MeasureTextEx(font, "<", 54.0f, 1.0f).x, btnRec.y + 10 }, 54.0f, 1.0f, WHITE);
                    }
                    DrawTextEx(font, tabNames[i], { SCREEN_W / 2.0f - sz.x / 2.0f, startY + i * spacing }, 54.0f, 1.0f, c);
                }
            }
            else // insideTab == true
            {
                if (IsKeyPressed(KEY_ESCAPE)) insideTab = false;
                
                // Back button generic logic
                Rectangle backBtn = { 50, 50, 180, 60 };
                if (CheckCollisionPointRec(GetMousePosition(), backBtn)) {
                    DrawRectangleRec(backBtn, {255,255,255,40});
                    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) insideTab = false;
                }
                DrawRectangleLinesEx(backBtn, 2.0f, WHITE);
                DrawTextEx(font, "ESC - Назад", { backBtn.x + 15, backBtn.y + 15 }, 30.0f, 1.0f, WHITE);

                switch (currentTab)
                {
                case MenuTab::PLAY:
                {
                    cTxt("ВЫБОР УРОВНЯ", 300, 58, { 255, 230, 80, 255 });
                    
                    if (IsKeyPressed(KEY_LEFT)  || IsKeyPressed(KEY_A))
                        selectedLevel = (selectedLevel + LEVEL_COUNT - 1) % LEVEL_COUNT;
                    if (IsKeyPressed(KEY_RIGHT) || IsKeyPressed(KEY_D))
                        selectedLevel = (selectedLevel + 1) % LEVEL_COUNT;
                    if (IsKeyPressed(KEY_ENTER)) resetGame(selectedLevel);

                    Vector2 mouse = GetMousePosition();
                    int prevLevel = (selectedLevel + LEVEL_COUNT - 1) % LEVEL_COUNT;
                    int nextLevel = (selectedLevel + 1) % LEVEL_COUNT;
                    const char* diff = (levelChartOk[selectedLevel] && !levelTemplates[selectedLevel].difficulty.empty())
                        ? levelTemplates[selectedLevel].difficulty.c_str()
                        : LEVEL_DEFS[selectedLevel].difficulty;
                    std::string highScoreText = "Рекорд: " + std::to_string(highScores[selectedLevel]);

                    Rectangle leftCard = { 320, 424, 292, 240 };
                    Rectangle centerCard = { 650, 376, 620, 366 };
                    Rectangle rightCard = { 1308, 424, 292, 240 };
                    Rectangle leftArrow = { centerCard.x - 82, centerCard.y + centerCard.height * 0.5f - 38, 56, 76 };
                    Rectangle rightArrow = { centerCard.x + centerCard.width + 26, centerCard.y + centerCard.height * 0.5f - 38, 56, 76 };
                    Rectangle startBtn = { centerCard.x + 160, centerCard.y + centerCard.height - 76, 300, 58 };

                    bool hoverLeftCard = CheckCollisionPointRec(mouse, leftCard);
                    bool hoverCenterCard = CheckCollisionPointRec(mouse, centerCard);
                    bool hoverRightCard = CheckCollisionPointRec(mouse, rightCard);
                    bool hoverLeftArrow = CheckCollisionPointRec(mouse, leftArrow);
                    bool hoverRightArrow = CheckCollisionPointRec(mouse, rightArrow);
                    bool hoverStartBtn = CheckCollisionPointRec(mouse, startBtn);

                    if ((hoverLeftArrow || hoverLeftCard) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
                        selectedLevel = prevLevel;
                    if ((hoverRightArrow || hoverRightCard) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
                        selectedLevel = nextLevel;
                    if ((hoverStartBtn || hoverCenterCard) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
                        resetGame(selectedLevel);

                    auto drawLevelPreview = [&](Rectangle rec, int levelIdx, bool active)
                    {
                        Color border = active ? Color{ 145, 170, 255, 230 } : Color{ 70, 82, 135, 150 };
                        Color fill = active ? Color{ 12, 16, 30, 240 } : Color{ 8, 10, 20, 165 };
                        DrawRectangleRounded(rec, 0.08f, 16, fill);
                        DrawRectangleRoundedLinesEx(rec, 0.08f, 16, 2.0f, border);
                        DrawRectangleGradientV((int)rec.x, (int)rec.y, (int)rec.width, (int)rec.height,
                            { LANE_COLORS[0].r, LANE_COLORS[0].g, LANE_COLORS[0].b, active ? (unsigned char)18 : (unsigned char)8 },
                            { 5, 8, 18, 0 });

                        std::string levelLabel = "УРОВЕНЬ " + std::to_string(levelIdx + 1);
                        Vector2 labelSz = MeasureTextEx(font, levelLabel.c_str(), active ? 70.0f : 38.0f, 1.0f);
                        DrawTextEx(font, levelLabel.c_str(),
                            { rec.x + rec.width * 0.5f - labelSz.x * 0.5f, rec.y + (active ? 56.0f : 72.0f) },
                            active ? 70.0f : 38.0f, 1.0f,
                            active ? WHITE : Color{ 180, 188, 230, 220 });

                        const char* previewDiff = (levelChartOk[levelIdx] && !levelTemplates[levelIdx].difficulty.empty())
                            ? levelTemplates[levelIdx].difficulty.c_str()
                            : LEVEL_DEFS[levelIdx].difficulty;
                        Vector2 diffSz = MeasureTextEx(font, previewDiff, active ? 40.0f : 28.0f, 1.0f);
                        DrawTextEx(font, previewDiff,
                            { rec.x + rec.width * 0.5f - diffSz.x * 0.5f, rec.y + (active ? 170.0f : 146.0f) },
                            active ? 40.0f : 28.0f, 1.0f, LEVEL_DEFS[levelIdx].diffColor);
                    };

                    drawLevelPreview(leftCard, prevLevel, false);
                    drawLevelPreview(centerCard, selectedLevel, true);
                    drawLevelPreview(rightCard, nextLevel, false);

                    DrawRectangleRounded(leftArrow, 0.18f, 10, hoverLeftArrow ? Color{ 65, 80, 155, 55 } : Color{ 20, 24, 40, 28 });
                    DrawRectangleRoundedLinesEx(leftArrow, 0.18f, 10, 2.0f, hoverLeftArrow ? Color{ 180, 200, 255, 255 } : Color{ 90, 100, 155, 160 });
                    DrawRectangleRounded(rightArrow, 0.18f, 10, hoverRightArrow ? Color{ 65, 80, 155, 55 } : Color{ 20, 24, 40, 28 });
                    DrawRectangleRoundedLinesEx(rightArrow, 0.18f, 10, 2.0f, hoverRightArrow ? Color{ 180, 200, 255, 255 } : Color{ 90, 100, 155, 160 });
                    DrawTextEx(font, "<", { leftArrow.x + 18.0f, leftArrow.y + 8.0f }, 52.0f, 1.0f, hoverLeftArrow ? WHITE : Color{ 185, 195, 235, 235 });
                    DrawTextEx(font, ">", { rightArrow.x + 18.0f, rightArrow.y + 8.0f }, 52.0f, 1.0f, hoverRightArrow ? WHITE : Color{ 185, 195, 235, 235 });

                    Vector2 highScoreSz = MeasureTextEx(font, highScoreText.c_str(), 30.0f, 1.0f);
                    DrawTextEx(font, highScoreText.c_str(),
                        { centerCard.x + centerCard.width * 0.5f - highScoreSz.x * 0.5f, centerCard.y + 226.0f },
                        30.0f, 1.0f, { 220, 220, 220, 255 });

                    DrawRectangleRounded(startBtn, 0.18f, 12, hoverStartBtn ? Color{ 85, 110, 255, 65 } : Color{ 35, 45, 95, 42 });
                    DrawRectangleRoundedLinesEx(startBtn, 0.18f, 12, 2.0f, hoverStartBtn ? Color{ 205, 220, 255, 255 } : Color{ 120, 140, 220, 220 });
                    Vector2 startSz = MeasureTextEx(font, "НАЧАТЬ", 34.0f, 1.0f);
                    DrawTextEx(font, "НАЧАТЬ",
                        { startBtn.x + startBtn.width * 0.5f - startSz.x * 0.5f, startBtn.y + 10.0f },
                        34.0f, 1.0f, hoverStartBtn ? WHITE : Color{ 230, 235, 255, 255 });

                    Vector2 hintSz = MeasureTextEx(font, "A / D ИЛИ СТРЕЛКИ  |  ENTER - ИГРАТЬ", 28.0f, 1.0f);
                    DrawTextEx(font, "A / D ИЛИ СТРЕЛКИ  |  ENTER - ИГРАТЬ",
                        { SCREEN_W * 0.5f - hintSz.x * 0.5f, 786.0f }, 28.0f, 1.0f, { 170, 178, 225, 220 });

                    if (!levelChartOk[selectedLevel])
                        cTxt(("ОШИБКА: " + std::string(LEVEL_DEFS[selectedLevel].chartPath) + " не найден").c_str(), 804, 30, RED);
                    if (!std::ifstream(LEVEL_DEFS[selectedLevel].musicPath).good())
                        cTxt(("ОШИБКА: " + std::string(LEVEL_DEFS[selectedLevel].musicPath) + " не найден").c_str(), 840, 30, RED);
                    break;
                }
                case MenuTab::CONTROLS:
                {
                    cTxt("УПРАВЛЕНИЕ", 285, 72, { 255, 230, 80, 255 });

                    const Rectangle heroPanel = { 260, 354, 1400, 528 };
                    const Rectangle keysPanel = { heroPanel.x + 46, heroPanel.y + 46, 420, 430 };
                    const Rectangle rulesPanel = { keysPanel.x + keysPanel.width + 40, keysPanel.y, 848, 430 };

                    auto drawPanel = [&](Rectangle rec, Color fill, Color border)
                    {
                        DrawRectangleRounded(rec, 0.08f, 18, fill);
                        DrawRectangleRoundedLinesEx(rec, 0.08f, 18, 2.0f, border);
                    };
                    auto drawCentered = [&](const char* text, Rectangle rec, float size, Color col)
                    {
                        Vector2 sz = MeasureTextEx(font, text, size, 1.0f);
                        DrawTextEx(font, text,
                            { rec.x + rec.width * 0.5f - sz.x * 0.5f, rec.y + rec.height * 0.5f - sz.y * 0.5f },
                            size, 1.0f, col);
                    };
                    auto drawKeyCap = [&](Rectangle rec, const char* key, Color glow)
                    {
                        DrawRectangleRounded(rec, 0.22f, 14, { 18, 24, 44, 235 });
                        DrawRectangleRoundedLinesEx(rec, 0.22f, 14, 2.0f, glow);
                        DrawRectangleGradientV((int)rec.x, (int)rec.y, (int)rec.width, (int)rec.height,
                            { glow.r, glow.g, glow.b, 26 }, { 0, 0, 0, 0 });
                        drawCentered(key, rec, 52.0f, WHITE);
                    };

                    drawPanel(heroPanel, { 10, 14, 28, 212 }, { 80, 96, 170, 200 });
                    DrawRectangleGradientV((int)heroPanel.x, (int)heroPanel.y, (int)heroPanel.width, 120,
                        { 70, 92, 170, 42 }, { 0, 0, 0, 0 });

                    drawPanel(keysPanel, { 16, 21, 38, 228 }, { 94, 112, 188, 215 });
                    DrawTextEx(font, "КЛАВИШИ", { keysPanel.x + 34, keysPanel.y + 28 }, 38.0f, 1.0f, WHITE);
                    DrawTextEx(font, "Каждая кнопка отвечает", { keysPanel.x + 34, keysPanel.y + 78 }, 22.0f, 1.0f, { 168, 180, 228, 220 });
                    DrawTextEx(font, "за свою линию", { keysPanel.x + 34, keysPanel.y + 108 }, 22.0f, 1.0f, { 168, 180, 228, 220 });

                    const float keyY = keysPanel.y + 174.0f;
                    const float keyW = 92.0f;
                    const float keyH = 102.0f;
                    const float keyGap = 30.0f;
                    const float keyStartX = keysPanel.x + 38.0f;
                    const char* keyNames[] = { "A", "S", "D" };
                    const Color keyColors[] = {
                        { 90, 180, 255, 255 },
                        { 100, 240, 165, 255 },
                        { 255, 110, 110, 255 }
                    };

                    for (int i = 0; i < 3; ++i) {
                        Rectangle keyRec = { keyStartX + i * (keyW + keyGap), keyY, keyW, keyH };
                        drawKeyCap(keyRec, keyNames[i], keyColors[i]);
                    }

                    DrawTextEx(font, "Жми, когда нота доходит", { keysPanel.x + 34, keysPanel.y + 314 }, 24.0f, 1.0f, WHITE);
                    DrawTextEx(font, "до линии удара.", { keysPanel.x + 34, keysPanel.y + 344 }, 24.0f, 1.0f, WHITE);
                    DrawTextEx(font, "Чем точнее тайминг,", { keysPanel.x + 34, keysPanel.y + 378 }, 21.0f, 1.0f, { 168, 180, 228, 220 });
                    DrawTextEx(font, "тем выше счёт и комбо.", { keysPanel.x + 34, keysPanel.y + 406 }, 21.0f, 1.0f, { 168, 180, 228, 220 });

                    drawPanel(rulesPanel, { 14, 18, 34, 228 }, { 94, 112, 188, 215 });
                    DrawTextEx(font, "ТИПЫ НОТ", { rulesPanel.x + 36, rulesPanel.y + 28 }, 38.0f, 1.0f, WHITE);
                    DrawTextEx(font, "Запомни три основных правила", { rulesPanel.x + 36, rulesPanel.y + 78 }, 22.0f, 1.0f, { 168, 180, 228, 220 });

                    auto drawNoteRule = [&](Rectangle rec, Color accent, const char* title, const char* line1, const char* line2, int kind)
                    {
                        drawPanel(rec, { 18, 24, 42, 220 }, { accent.r, accent.g, accent.b, 210 });
                        Rectangle iconZone = { rec.x + 24, rec.y + 15, 74, 74 };
                        Vector2 center = { iconZone.x + iconZone.width * 0.5f, iconZone.y + iconZone.height * 0.5f };

                        DrawCircleV(center, 20.0f, accent);
                        DrawRing(center, 24.0f, 30.0f, 0.0f, 360.0f, 36, { accent.r, accent.g, accent.b, 70 });

                        if (kind == 1) {
                            DrawTextEx(font, "!", { center.x - 5.0f, center.y - 19.0f }, 34.0f, 1.0f, WHITE);
                        }
                        else if (kind == 2) {
                            DrawRectangleRounded({ center.x + 18.0f, center.y - 7.0f, 86.0f, 14.0f }, 0.5f, 10, { accent.r, accent.g, accent.b, 170 });
                            DrawCircleV({ center.x + 110.0f, center.y }, 10.0f, accent);
                        }

                        DrawTextEx(font, title, { rec.x + 122, rec.y + 14 }, 28.0f, 1.0f, WHITE);
                        DrawTextEx(font, line1, { rec.x + 122, rec.y + 48 }, 21.0f, 1.0f, { 214, 220, 245, 240 });
                        DrawTextEx(font, line2, { rec.x + 122, rec.y + 70 }, 21.0f, 1.0f, { 168, 180, 228, 220 });
                    };

                    const float ruleGap = 14.0f;
                    const float ruleX = rulesPanel.x + 30.0f;
                    const float ruleY = rulesPanel.y + 114.0f;
                    const float ruleW = rulesPanel.width - 60.0f;
                    const float ruleH = 92.0f;

                    drawNoteRule({ ruleX, ruleY, ruleW, ruleH },
                        { 90, 180, 255, 255 },
                        "ОБЫЧНЫЕ",
                        "Синие, зелёные и красные",
                        "Нажми точно в ритм.",
                        0);
                    drawNoteRule({ ruleX, ruleY + ruleH + ruleGap, ruleW, ruleH },
                        { 255, 95, 95, 255 },
                        "ЛОВУШКИ",
                        "Красные с восклицательным знаком",
                        "Их нужно пропускать.",
                        1);
                    drawNoteRule({ ruleX, ruleY + (ruleH + ruleGap) * 2.0f, ruleW, ruleH },
                        { 200, 95, 255, 255 },
                        "УДЕРЖАНИЕ",
                        "Фиолетовые длинные ноты",
                        "Держи клавишу до конца.",
                        2);

                    break;
                }
                case MenuTab::ABOUT:
                {
                    cTxt("ОБ АВТОРЕ", 330, 64, { 255, 230, 80, 255 });
                    cTxt("Проект: Ритм-игра Синхрон", 460, 42, WHITE);
                    cTxt("Разработчик: Лисовский Евгений Витальевич", 520, 42, WHITE);
                    cTxt("Группа: ПЗТ-45", 580, 42, WHITE);
                    cTxt("Технологии: C++, Raylib", 640, 42, WHITE);
                    cTxt("Год разработки: 2026", 700, 42, WHITE);
                    break;
                }
                case MenuTab::SETTINGS:
                {
                    cTxt("НАСТРОЙКИ", 330, 64, { 255, 230, 80, 255 });

                    if (IsKeyPressed(KEY_UP) || IsKeyPressed(KEY_W)) settingsSelection = (settingsSelection + 2) % 3;
                    if (IsKeyPressed(KEY_DOWN) || IsKeyPressed(KEY_S)) settingsSelection = (settingsSelection + 1) % 3;

                    const char* sNames[] = { "Громкость", "Скорость нот", "Полный экран" };
                    float startY = 460.0f;
                    float spacing = 80.0f;
                    Vector2 mouse = GetMousePosition();

                    for (int i = 0; i < 3; i++) {
                        float y = startY + i * spacing;
                        Color c = (settingsSelection == i) ? WHITE : Color{ 160, 160, 220, 200 };
                        
                        Rectangle rowRec = { SCREEN_W / 2.0f - 400, y - 10, 800, 60 };
                        if (CheckCollisionPointRec(mouse, rowRec)) {
                            settingsSelection = i;
                        }

                        DrawTextEx(font, sNames[i], { SCREEN_W / 2.0f - 300, y }, 42.0f, 1.0f, c);

                        // Value display and controls
                        char valBuf[32];
                        if (i == 0) snprintf(valBuf, sizeof(valBuf), "%d%%", (int)(globalVolume * 100.0f + 0.5f));
                        else if (i == 1) snprintf(valBuf, sizeof(valBuf), "%.2f", noteSpeed);
                        else snprintf(valBuf, sizeof(valBuf), isFullscreen ? "ВКЛ" : "ВЫКЛ");

                        Vector2 vSz = MeasureTextEx(font, valBuf, 42.0f, 1.0f);
                        float vX = SCREEN_W / 2.0f + 150.0f;
                        
                        Rectangle leftArr = { vX - 50, y - 5, 40, 50 };
                        Rectangle rightArr = { vX + vSz.x + 10, y - 5, 40, 50 };

                        if (settingsSelection == i) {
                            DrawTextEx(font, "<", { leftArr.x, y }, 42.0f, 1.0f, CheckCollisionPointRec(mouse, leftArr) ? WHITE : c);
                            DrawTextEx(font, ">", { rightArr.x, y }, 42.0f, 1.0f, CheckCollisionPointRec(mouse, rightArr) ? WHITE : c);
                            
                            bool leftHit = IsKeyPressed(KEY_LEFT) || IsKeyPressed(KEY_A) || (CheckCollisionPointRec(mouse, leftArr) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT));
                            bool rightHit = IsKeyPressed(KEY_RIGHT) || IsKeyPressed(KEY_D) || (CheckCollisionPointRec(mouse, rightArr) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT));

                            if (i == 0) { // Volume
                                if (leftHit && globalVolume > 0.0f) globalVolume -= 0.1f;
                                if (rightHit && globalVolume < 1.0f) globalVolume += 0.1f;
                                if (globalVolume < 0.0f) globalVolume = 0.0f;
                                if (globalVolume > 1.0f) globalVolume = 1.0f;
                                if (leftHit || rightHit) SetMusicVolume(music, globalVolume);
                            }
                            else if (i == 1) { // Speed
                                if (leftHit && noteSpeed > 0.2f) noteSpeed -= 0.05f;
                                if (rightHit && noteSpeed < 2.0f) noteSpeed += 0.05f;
                            }
                            else if (i == 2) { // Fullscreen
                                if (leftHit || rightHit) {
                                    ToggleFullscreen();
                                    isFullscreen = IsWindowFullscreen();
                                }
                            }
                        }

                        DrawTextEx(font, valBuf, { vX, y }, 42.0f, 1.0f, c);
                    }
                    break;
                }
                } // switch(currentTab)
            }
        }
        // ================================================================
        // GAMEPLAY
        // ================================================================
        else if (state == GameState::GAMEPLAY || state == GameState::PAUSED)
        {
            // --- Shared time ---
            if (state == GameState::GAMEPLAY && musicOk) UpdateMusicStream(music);
            float currentTimeMs = musicOk ? GetMusicTimePlayed(music) * 1000.0f : 0.0f;
            hudCurrentTimeMs = currentTimeMs;

            if (state == GameState::GAMEPLAY) {
                visualTimeMs += dt * 1000.0f;
                // Эквивалент Lerp(visualTimeMs, currentTimeMs, 10.0f * dt)
                visualTimeMs += (currentTimeMs - visualTimeMs) * 10.0f * dt;
            } else if (state == GameState::PAUSED) {
                visualTimeMs = currentTimeMs;
            }

            // --- Update (only during GAMEPLAY) ---
            if (state == GameState::GAMEPLAY)
            {
                // ESC → pause
                if (IsKeyPressed(KEY_ESCAPE))
                {
                    if (musicOk) PauseMusicStream(music);
                    pauseSelection = 0;
                    justPaused = true;
                    state = GameState::PAUSED;
                }

                // --- Fade-out transition ---
                if (fadingOut)
                {
                    fadeOutTimer -= dt;
                    if (musicOk) SetMusicVolume(music, (fadeOutTimer > 0.0f ? fadeOutTimer / FADE_OUT_DURATION : 0.0f) * globalVolume);
                    if (fadeOutTimer <= 0.0f)
                    {
                        fadingOut = false;
                        if (musicOk) { StopMusicStream(music); SetMusicVolume(music, globalVolume); }
                        
                        if (score > highScores[currentLevel]) {
                            highScores[currentLevel] = score;
                            saveHighScores();
                        }
                        
                        state = GameState::RESULT;
                    }
                }

                // Auto-miss: normal/hold-idle notes that passed start window
                // Trap notes that passed → silent deactivation
                if (!fadingOut) for (auto& note : notes)
                {
                    if (!note.active) continue;
                    if (note.type == NoteType::HOLD && note.holdState == HoldState::HOLDING) continue;
                    if (note.type == NoteType::HOLD && note.holdState == HoldState::FAILED)  continue;
                    if (note.scrollingOff) continue;
                    if ((currentTimeMs - (float)note.time_ms) > (float)HIT_WINDOW_MISS)
                    {
                        if (note.type == NoteType::HOLD && note.holdState == HoldState::IDLE)
                        {
                            note.holdState = HoldState::FAILED; // miss recorded, keep visible until scrolled off
                            missCount++;
                            combo = 0;
                            lastJudgment = Judgment::MISS;
                            judgmentTimer = 1.5f;
                        }
                        else
                        {
                            note.scrollingOff = true; // keep visible until off screen
                            if (note.type != NoteType::TRAP)
                            {
                                missCount++;
                                combo = 0;
                                lastJudgment = Judgment::MISS;
                                judgmentTimer = 1.5f;
                            }
                        }
                    }
                }

                // HOLDING notes: check key still down each frame
                if (!fadingOut) for (auto& note : notes)
                {
                    if (!note.active)                         continue;
                    if (note.type != NoteType::HOLD)     continue;
                    if (note.holdState != HoldState::HOLDING) continue;

                    bool keyDown = (note.lane == 1 && IsKeyDown(KEY_A)) ||
                        (note.lane == 2 && IsKeyDown(KEY_S)) ||
                        (note.lane == 3 && IsKeyDown(KEY_D));
                    float endMs = (float)(note.time_ms + note.duration_ms);

                    if (currentTimeMs >= endMs)
                    {
                        note.active = false;
                        note.holdState = HoldState::COMPLETED;
                        score += 150 * getComboMultiplier(combo);
                        holdOkCount++;
                        combo++;
                        if (combo > maxCombo) maxCombo = combo;
                        lastJudgment = Judgment::HOLD_OK;
                        judgmentTimer = 1.5f;
                    }
                    else if (!keyDown)
                    {
                        note.active = false;
                        note.holdState = HoldState::FAILED;
                        holdMissCount++;
                        combo = 0;
                        lastJudgment = Judgment::HOLD_MISS;
                        judgmentTimer = 1.5f;
                    }
                }

                // Input: A=lane1, S=lane2, D=lane3
                auto processLane = [&](int lane)
                    {
                        // Find the earliest active, non-holding note within the miss window
                        int   bestIdx = -1;
                        float bestTime = 1e30f;
                        for (int i = 0; i < (int)notes.size(); i++)
                        {
                            if (!notes[i].active)       continue;
                            if (notes[i].scrollingOff)  continue;
                            if (notes[i].lane != lane)  continue;
                            if (notes[i].type == NoteType::HOLD &&
                                (notes[i].holdState == HoldState::HOLDING ||
                                 notes[i].holdState == HoldState::FAILED)) continue;
                            float d = currentTimeMs - (float)notes[i].time_ms;
                            float absDiff = d < 0.0f ? -d : d;
                            if (absDiff > (float)HIT_WINDOW_MISS) continue;
                            if ((float)notes[i].time_ms < bestTime)
                            {
                                bestTime = (float)notes[i].time_ms;
                                bestIdx = i;
                            }
                        }

                        // Ghost tap: no note within the miss window
                        if (bestIdx == -1)
                        {
                            score = (score >= 50) ? score - 50 : 0;
                            missCount++;
                            combo = 0;
                            lastJudgment = Judgment::MISS;
                            judgmentTimer = 1.5f;
                            return;
                        }

                        float d = currentTimeMs - (float)notes[bestIdx].time_ms;
                        float absDiff = d < 0.0f ? -d : d;

                        if (notes[bestIdx].type == NoteType::TRAP)
                        {
                            notes[bestIdx].active = false;
                            score = (score >= 100) ? score - 100 : 0;
                            trapErrors++;
                            combo = 0;
                            lastJudgment = Judgment::TRAP;
                            judgmentTimer = 1.5f;
                            flashAlpha = (currentLevel == 2) ? 1.0f : 0.5f;
                            shakeTime = 0.3f;
                        }
                        else if (notes[bestIdx].type == NoteType::HOLD &&
                            notes[bestIdx].holdState == HoldState::IDLE)
                        {
                            if (absDiff <= (float)HIT_WINDOW_GOOD)
                            {
                                notes[bestIdx].holdState = HoldState::HOLDING;
                                if (absDiff <= (float)HIT_WINDOW_PERFECT) { score += 100 * getComboMultiplier(combo); perfectCount++; lastJudgment = Judgment::PERFECT; hitEffect[lane-1] = 0.15f; hitEffectPerfect[lane-1] = true; comboPopTimer = scorePopTimer = 0.18f; comboPopPerf = scorePopPerf = true; }
                                else { score += 50 * getComboMultiplier(combo); goodCount++; lastJudgment = Judgment::GOOD; hitEffect[lane-1] = 0.12f; hitEffectPerfect[lane-1] = false; comboPopTimer = scorePopTimer = 0.18f; comboPopPerf = scorePopPerf = false; }
                                combo++;
                                if (combo > maxCombo) maxCombo = combo;
                                judgmentTimer = 1.0f;
                            }
                        }
                        else if (absDiff <= (float)HIT_WINDOW_PERFECT)
                        {
                            notes[bestIdx].active = false;
                            score += 100 * getComboMultiplier(combo); perfectCount++;
                            combo++; if (combo > maxCombo) maxCombo = combo;
                            lastJudgment = Judgment::PERFECT;
                            judgmentTimer = 1.5f;
                            hitEffect[lane-1] = 0.15f; hitEffectPerfect[lane-1] = true;
                            comboPopTimer = scorePopTimer = 0.18f; comboPopPerf = scorePopPerf = true;
                        }
                        else if (absDiff <= (float)HIT_WINDOW_GOOD)
                        {
                            notes[bestIdx].active = false;
                            score += 50 * getComboMultiplier(combo); goodCount++;
                            combo++; if (combo > maxCombo) maxCombo = combo;
                            lastJudgment = Judgment::GOOD;
                            judgmentTimer = 1.5f;
                            hitEffect[lane-1] = 0.12f; hitEffectPerfect[lane-1] = false;
                            comboPopTimer = scorePopTimer = 0.18f; comboPopPerf = scorePopPerf = false;
                        }
                        // else: note exists but is outside GOOD window — press ignored, note keeps falling
                    };

                if (!fadingOut) {
                    if (IsKeyPressed(KEY_A)) processLane(1);
                    if (IsKeyPressed(KEY_S)) processLane(2);
                    if (IsKeyPressed(KEY_D)) processLane(3);
                }

                // Update lane glow based on key states (Smooth Lerp)
                if (!fadingOut) {
                    bool isLaneDown[3] = { IsKeyDown(KEY_A), IsKeyDown(KEY_S), IsKeyDown(KEY_D) };
                    for (int i = 0; i < 3; i++) {
                        if (isLaneDown[i]) {
                            laneGlow[i] += (0.5f - laneGlow[i]) * 10.0f * dt;
                        } else {
                            laneGlow[i] += (0.0f - laneGlow[i]) * 6.0f * dt;
                        }
                    }
                }

                if (judgmentTimer > 0.0f) judgmentTimer -= dt;
                for (int i = 0; i < 3; i++) if (hitEffect[i] > 0.0f) { hitEffect[i] -= dt; if (hitEffect[i] < 0.0f) hitEffect[i] = 0.0f; }
                if (comboPopTimer > 0.0f) { comboPopTimer -= dt; if (comboPopTimer < 0.0f) comboPopTimer = 0.0f; }
                if (scorePopTimer > 0.0f) { scorePopTimer -= dt; if (scorePopTimer < 0.0f) scorePopTimer = 0.0f; }
                if (flashAlpha > 0.0f) { float fDecay = (currentLevel == 2) ? 10.0f : 1.5f; flashAlpha -= dt * fDecay; if (flashAlpha < 0.0f) flashAlpha = 0.0f; }
                if (shakeTime > 0.0f) { shakeTime -= dt; shakeOffsetX = GetRandomValue(-5, 5); shakeOffsetY = GetRandomValue(-5, 5); }
                else { shakeOffsetX = 0; shakeOffsetY = 0; }

                // Song end: start fade when <=2.5s remain in track
                if (!fadingOut && musicOk)
                {
                    float trackLen  = GetMusicTimeLength(music);
                    float trackTime = GetMusicTimePlayed(music);
                    float remaining = trackLen - trackTime;
                    if (remaining <= FADE_OUT_DURATION && trackTime > 0.5f)
                    { fadingOut = true; fadeOutTimer = remaining > 0.0f ? remaining : 0.01f; }
                }
            } // end if (state == GAMEPLAY) update block

            // --- Draw gameplay ---
            // Background gradient (removed or tinted with bg_color if needed)
            // Just clearing it is done at the beginning of the frame.
            bool billieJeanSpotlight = (currentLevel == 3);
            float spotlightLane[3] = { 0.0f, 0.0f, 0.0f };
            float beamCenterA = SCREEN_W * 0.5f;
            float beamCenterB = SCREEN_W * 0.5f;
            if (billieJeanSpotlight)
            {
                DrawRectangle(0, 0, SCREEN_W, SCREEN_H, { 2, 2, 6, 255 });

                float spotPhase = visualTimeMs * 0.0017f;
                beamCenterA = SCREEN_W * 0.5f + sinf(spotPhase) * 320.0f;
                beamCenterB = SCREEN_W * 0.5f + sinf(spotPhase * 0.82f + 1.7f) * 270.0f;

                for (int i = 0; i < 3; i++)
                {
                    float distA = fabsf(LANE_CENTERS[i] - beamCenterA);
                    float distB = fabsf(LANE_CENTERS[i] - beamCenterB);
                    float influenceA = 1.0f - distA / 260.0f;
                    float influenceB = 1.0f - distB / 260.0f;
                    if (influenceA < 0.0f) influenceA = 0.0f;
                    if (influenceB < 0.0f) influenceB = 0.0f;
                    spotlightLane[i] = influenceA > influenceB ? influenceA : influenceB;
                }
            }
            
            // Vertical lane columns — layered for subtle depth
            for (int i = 0; i < 3; i++)
            {
                float cx = LANE_CENTERS[i] + (float)shakeOffsetX;
                int   lx = (int)(cx - LANE_WIDTH * 0.5f);
                int   lw = (int)LANE_WIDTH;
                Color lc = LANE_COLORS[i];

                // Base: vertical gradient — darker at top, slightly warmer at bottom
                if (billieJeanSpotlight)
                    DrawRectangleGradientV(lx, 0, lw, SCREEN_H, { 8, 8, 12, 215 }, { 12, 12, 18, 232 });
                else
                    DrawRectangleGradientV(lx, 0, lw, SCREEN_H, { 20, 24, 34, 188 }, { 30, 36, 50, 200 });

                // NEW: Soft full-height neon lane glow
                if (laneGlow[i] > 0.01f) {
                    Color transparentColor = ColorAlpha(lc, 0.0f);
                    Color glowColor = ColorAlpha(lc, laneGlow[i]);
                    
                    // Top part: from y=0 to HIT_Y
                    DrawRectangleGradientV(lx, 0, lw, (int)HIT_Y, transparentColor, glowColor);
                    
                    // Bottom part: from HIT_Y to SCREEN_H
                    DrawRectangleGradientV(lx, (int)HIT_Y, lw, SCREEN_H - (int)HIT_Y, glowColor, transparentColor);

                    if (currentTileGlow) {
                        int tileH = 120;
                        int tileY = (int)HIT_Y - tileH / 2;
                        Color tileColor = { 255, 255, 255, (unsigned char)(laneGlow[i] * 255.0f) };
                        Color tileCore = { lc.r, lc.g, lc.b, (unsigned char)(laneGlow[i] * 255.0f) };
                        DrawRectangle(lx + 4, tileY, lw - 8, tileH, tileCore);
                        DrawRectangleLines(lx + 4, tileY, lw - 8, tileH, tileColor);
                    }
                }

                // Soft center glow — faint lane-colour tint
                DrawRectangleGradientH(lx,          0, lw / 2, SCREEN_H, { lc.r, lc.g, lc.b,  0 }, { lc.r, lc.g, lc.b, 20 });
                DrawRectangleGradientH(lx + lw / 2, 0, lw / 2, SCREEN_H, { lc.r, lc.g, lc.b, 20 }, { lc.r, lc.g, lc.b,  0 });

                // Inner edge bevel — white highlight just inside each border
                DrawRectangleGradientH(lx,              0, 8, SCREEN_H, { 200, 210, 240, 30 }, { 200, 210, 240,  0 });
                DrawRectangleGradientH(lx + lw - 8,     0, 8, SCREEN_H, { 200, 210, 240,  0 }, { 200, 210, 240, 30 });

                // Visible border lines
                DrawLineEx({ cx - LANE_WIDTH * 0.5f, 0.0f }, { cx - LANE_WIDTH * 0.5f, (float)SCREEN_H },
                    1.5f, { 90, 100, 145, 110 });
                DrawLineEx({ cx + LANE_WIDTH * 0.5f, 0.0f }, { cx + LANE_WIDTH * 0.5f, (float)SCREEN_H },
                    1.5f, { 90, 100, 145, 110 });
            }

            if (billieJeanSpotlight)
            {
                float topY = -80.0f;
                float bottomY = SCREEN_H + 60.0f;
                float sourceA = SCREEN_W * 0.26f + sinf(visualTimeMs * 0.0011f) * 120.0f;
                float sourceB = SCREEN_W * 0.74f + sinf(visualTimeMs * 0.0010f + 2.1f) * 120.0f;

                BeginBlendMode(BLEND_ADDITIVE);
                DrawTriangle({ sourceA - 84.0f, topY }, { sourceA + 84.0f, topY }, { beamCenterA, bottomY },
                    { 80, 120, 255, 26 });
                DrawTriangle({ sourceA - 40.0f, topY }, { sourceA + 40.0f, topY }, { beamCenterA, bottomY },
                    { 170, 205, 255, 18 });
                DrawTriangle({ sourceB - 84.0f, topY }, { sourceB + 84.0f, topY }, { beamCenterB, bottomY },
                    { 135, 80, 255, 24 });
                DrawTriangle({ sourceB - 40.0f, topY }, { sourceB + 40.0f, topY }, { beamCenterB, bottomY },
                    { 215, 185, 255, 16 });

                for (int i = 0; i < 3; i++)
                {
                    if (spotlightLane[i] <= 0.01f) continue;
                    float cx = LANE_CENTERS[i] + (float)shakeOffsetX;
                    int lx = (int)(cx - LANE_WIDTH * 0.5f);
                    unsigned char laneAlpha = (unsigned char)(spotlightLane[i] * 62.0f);
                    unsigned char coreAlpha = (unsigned char)(spotlightLane[i] * 34.0f);
                    DrawRectangleGradientV(lx - 24, 0, (int)LANE_WIDTH + 48, SCREEN_H,
                        { 70, 110, 255, 0 }, { 70, 110, 255, laneAlpha });
                    DrawRectangleGradientV(lx + 12, 0, (int)LANE_WIDTH - 24, SCREEN_H,
                        { 255, 255, 255, 0 }, { 255, 255, 255, coreAlpha });
                }
                EndBlendMode();
            }


            // Detect which lanes have active hold notes
            bool laneHolding[3] = { false, false, false };
            for (const auto& n : notes)
                if (n.active && n.type == NoteType::HOLD && n.holdState == HoldState::HOLDING)
                    laneHolding[n.lane - 1] = true;

            // Hit targets (colored rings at HIT_Y)
            const char* laneKeys[3] = { "A", "S", "D" };
            for (int i = 0; i < 3; i++)
            {
                float cx = LANE_CENTERS[i] + (float)shakeOffsetX;
                float hy = HIT_Y + (float)shakeOffsetY;
                Color lc = LANE_COLORS[i];
                // Extra glow when this lane is being held
                if (laneHolding[i])
                {
                    float hp = 0.5f + 0.5f * sinf(visualTimeMs * 0.007f);
                    DrawCircle((int)cx, (int)hy, (int)(93 + hp * 12), { 25, 190, 240, (unsigned char)(18 + (int)(hp * 22)) });
                    DrawCircle((int)cx, (int)hy, (int)(78 + hp *  9), { 25, 190, 240, (unsigned char)(32 + (int)(hp * 28)) });
                }
                DrawCircle((int)cx, (int)hy, 69, { lc.r, lc.g, lc.b, laneHolding[i] ? (unsigned char)55 : (unsigned char)35 });
                DrawRing({ cx, hy }, 46.5f, 58.5f, 0.0f, 360.0f, 72, laneHolding[i] ? Color{ 25, 210, 255, 255 } : lc);
                DrawCircle((int)cx, (int)hy, 42, { 30, 35, 38, 235 });
                DrawRing({ cx, hy }, 40.5f, 43.5f, 0.0f, 360.0f, 72, { 230, 235, 235, 55 });
                drawTxt(laneKeys[i], (int)(cx - 12.0f), (int)(hy + 69.0f), 36, { 235, 238, 238, 240 });
            }

            // Hit effects (expanding ring + flash on Perfect/Good)
            for (int i = 0; i < 3; i++)
            {
                if (hitEffect[i] <= 0.0f) continue;
                float cx  = LANE_CENTERS[i] + (float)shakeOffsetX;
                float hy  = HIT_Y + (float)shakeOffsetY;
                Color lc  = LANE_COLORS[i];
                float t   = hitEffect[i] / 0.15f;               // 1=fresh → 0=gone
                bool  perf = hitEffectPerfect[i];
                float expand = perf ? 60.0f : 36.0f;
                float ringR  = 66.0f + (1.0f - t) * expand;    // ring expands outward
                unsigned char ringA  = (unsigned char)(t * (perf ? 220 : 140));
                unsigned char flashA = (unsigned char)(t * t * (perf ? 80 : 45));
                // Expanding ring
                DrawRing({ cx, hy }, ringR - 5.25f, ringR + 5.25f, 0.0f, 360.0f, 48,
                    { lc.r, lc.g, lc.b, ringA });
                // Soft glow fill
                DrawCircle((int)cx, (int)hy, (int)ringR, { lc.r, lc.g, lc.b, flashA });
                // Perfect: extra white center burst
                if (perf)
                {
                    unsigned char burstA = (unsigned char)(t * t * 120);
                    DrawCircle((int)cx, (int)hy, (int)(45.0f + (1.0f - t) * 18.0f),
                        { 255, 255, 255, burstA });
                }
            }

            // Notes (fall top → bottom)
            for (auto& note : notes)
            {
                if (!note.active) continue;
                if (note.lane < 1 || note.lane > 3) continue;
                float cx = LANE_CENTERS[note.lane - 1] + (float)shakeOffsetX;
                float currentY = HIT_Y - ((float)note.time_ms - visualTimeMs) * noteSpeed + (float)shakeOffsetY;
                bool  isHolding = (note.type == NoteType::HOLD && note.holdState == HoldState::HOLDING);
                float bottomEdge = (note.type == NoteType::HOLD)
                    ? currentY - (float)note.duration_ms * noteSpeed  // tail (top of bar)
                    : currentY;
                if (!isHolding && (currentY < -210.0f || bottomEdge > SCREEN_H + 90.0f))
                {
                    if (note.type == NoteType::HOLD && note.holdState == HoldState::FAILED)
                        note.active = false;
                    if (note.scrollingOff)
                        note.active = false;
                    continue;
                }
                Color lc = LANE_COLORS[note.lane - 1];

                switch (note.type)
                {
                case NoteType::HOLD:
                {
                    float headY, tailY;
                    if (isHolding)
                    {
                        headY = HIT_Y;
                        float remainMs = (float)(note.time_ms + note.duration_ms) - visualTimeMs;
                        tailY = HIT_Y - remainMs * noteSpeed;
                    }
                    else
                    {
                        headY = currentY;
                        tailY = currentY - (float)note.duration_ms * noteSpeed;
                    }
                    float topY = tailY < headY ? tailY : headY;
                    float botY = tailY < headY ? headY : tailY;
                    float bodyH = botY - topY;
                    if (bodyH > 0)
                    {
                        if (isHolding)
                        {
                            float bp = 0.5f + 0.5f * sinf(visualTimeMs * 0.007f);
                            // Pulsing outer glow layers
                            DrawRectangle((int)(cx - 57.0f), (int)topY, 114, (int)bodyH,
                                { 25, 190, 240, (unsigned char)(10 + (int)(bp * 16)) });
                            DrawRectangle((int)(cx - 45.0f), (int)topY,  90, (int)bodyH,
                                { 25, 190, 240, (unsigned char)(22 + (int)(bp * 30)) });
                            DrawRectangle((int)(cx - 36.0f), (int)topY,  72, (int)bodyH,
                                { 25, 190, 240, (unsigned char)(38 + (int)(bp * 36)) });
                            // Main body
                            DrawRectangle((int)(cx - 27.0f), (int)topY,  54, (int)bodyH,
                                { 25, 200, 250, (unsigned char)(200 + (int)(bp * 40)) });
                            // Traveling highlight strip
                            float travelY = topY + fmodf(visualTimeMs * 0.15f, bodyH);
                            DrawRectangle((int)(cx - 21.0f), (int)travelY, 42, 27,
                                { 160, 235, 255, (unsigned char)(70 + (int)(bp * 80)) });
                            // Edge lines — pulsing
                            DrawLineEx({ cx - 33.0f, topY }, { cx - 33.0f, botY },
                                4.5f, { 80, 230, 255, (unsigned char)(190 + (int)(bp * 55)) });
                            DrawLineEx({ cx + 33.0f, topY }, { cx + 33.0f, botY },
                                4.5f, { 80, 230, 255, (unsigned char)(190 + (int)(bp * 55)) });
                        }
                        else
                        {
                            DrawRectangle((int)(cx - 45.0f), (int)topY,  90, (int)bodyH, { 190, 60, 255,  18 });
                            DrawRectangle((int)(cx - 36.0f), (int)topY,  72, (int)bodyH, { 190, 60, 255,  30 });
                            DrawRectangle((int)(cx - 27.0f), (int)topY,  54, (int)bodyH, { 190, 60, 255, 135 });
                            DrawLineEx({ cx - 33.0f, topY }, { cx - 33.0f, botY }, 4.5f, { 220, 120, 255, 210 });
                            DrawLineEx({ cx + 33.0f, topY }, { cx + 33.0f, botY }, 4.5f, { 220, 120, 255, 210 });
                        }
                    }
                    // Head (at bottom / HIT_Y when holding)
                    if (isHolding)
                    {
                        float hp = 0.5f + 0.5f * sinf(visualTimeMs * 0.007f);
                        DrawCircle((int)cx, (int)headY, (int)(105 + hp * 15), { 25, 190, 240, (unsigned char)(8  + (int)(hp * 12)) });
                        DrawCircle((int)cx, (int)headY, (int)( 84 + hp * 12), { 25, 190, 240, (unsigned char)(18 + (int)(hp * 22)) });
                        DrawCircle((int)cx, (int)headY, (int)( 66 + hp *  9), { 25, 200, 250, (unsigned char)(35 + (int)(hp * 30)) });
                        DrawRing({ cx, headY }, 33.0f, 48.0f, 0.0f, 360.0f, 72, { 80, 220, 255, 255 });
                        DrawCircle((int)cx, (int)headY, 27, { 15, 45, 60, 245 });
                        DrawRing({ cx, headY }, 18.0f, 25.5f, 0.0f, 360.0f, 72,
                            { 200, 245, 255, (unsigned char)(170 + (int)(hp * 70)) });
                    }
                    else
                    {
                        DrawCircle((int)cx, (int)headY, 84, { 200, 80, 255, 10 });
                        DrawCircle((int)cx, (int)headY, 69, { 200, 80, 255, 20 });
                        DrawCircle((int)cx, (int)headY, 54, { 200, 80, 255, 45 });
                        DrawRing({ cx, headY }, 33.0f, 45.0f, 0.0f, 360.0f, 72, { 220, 120, 255, 255 });
                        DrawCircle((int)cx, (int)headY, 26, { 50, 20, 70, 230 });
                    }
                    // Tail (top end) — filled circle plugs the inner hole, ring drawn on top
                    if (isHolding)
                    {
                        float hp = 0.5f + 0.5f * sinf(visualTimeMs * 0.007f);
                        DrawCircle((int)cx, (int)tailY, 26, { 25, 200, 250, (unsigned char)(200 + (int)(hp * 40)) });
                        DrawRing({ cx, tailY }, 26.0f, 34.5f, 0.0f, 360.0f, 72, { 80, 225, 255, 255 });
                    }
                    else
                    {
                        DrawCircle((int)cx, (int)tailY, 26, { 190, 60, 255, 135 });
                        DrawRing({ cx, tailY }, 26.0f, 34.5f, 0.0f, 360.0f, 72, { 220, 120, 255, 210 });
                    }
                    break;
                }
                case NoteType::TRAP:
                {
                    float pulse = 0.5f + 0.5f * sinf(visualTimeMs * 0.008f);
                    // Pulsing outer danger glow
                    DrawCircle((int)cx, (int)currentY, 87, { 255, 25,  8, (unsigned char)(15 + (int)(pulse * 25)) });
                    DrawCircle((int)cx, (int)currentY, 69, { 255, 50, 20, (unsigned char)(28 + (int)(pulse * 38)) });
                    DrawCircle((int)cx, (int)currentY, 54, { 255, 70, 30, (unsigned char)(45 + (int)(pulse * 45)) });
                    // Outer ring — color shifts orange→red with pulse
                    DrawRing({ cx, currentY }, 36.0f, 51.0f, 0.0f, 360.0f, 72,
                        { 255, (unsigned char)(55 + (int)(pulse * 45)), 25, 255 });
                    // Dark core
                    DrawCircle((int)cx, (int)currentY, 33, { 50, 14, 14, 252 });
                    // Inner accent ring
                    DrawRing({ cx, currentY }, 16.5f, 22.5f, 0.0f, 360.0f, 48,
                        { 255, (unsigned char)(145 + (int)(pulse * 65)), 45, 255 });
                    // "!" warning — flickers brighter with pulse
                    drawTxt("!", (int)(cx - 7.5f), (int)(currentY - 30.0f), 45,
                        { 255, (unsigned char)(100 + (int)(pulse * 110)), 50, 255 });
                    // 4 corner spikes radiating outward
                    {
                        float sR = 45.0f, sL = 13.5f;
                        unsigned char sA = (unsigned char)(170 + (int)(pulse * 70));
                        for (int d = 0; d < 4; d++)
                        {
                            float ang = (45.0f + d * 90.0f) * DEG2RAD;
                            DrawLineEx({ cx + cosf(ang) * sR,        currentY + sinf(ang) * sR        },
                                       { cx + cosf(ang) * (sR + sL), currentY + sinf(ang) * (sR + sL) },
                                       4.5f, { 255, 65, 25, sA });
                        }
                    }
                    break;
                }
                default: // NORMAL
                    DrawCircle((int)cx, (int)currentY, 78, { lc.r, lc.g, lc.b, 12 });
                    DrawCircle((int)cx, (int)currentY, 63, { lc.r, lc.g, lc.b, 22 });
                    DrawCircle((int)cx, (int)currentY, 51, { lc.r, lc.g, lc.b, 45 });
                    DrawRing({ cx, currentY }, 25.5f, 36.0f, 0.0f, 360.0f, 72, lc);
                    DrawCircle((int)cx, (int)currentY, 21, { 38, 45, 48, 240 });
                    DrawRing({ cx, currentY }, 19.5f, 22.5f, 0.0f, 360.0f, 72, { 245, 250, 250, 70 });
                    break;
                }
            }

            // Screen flash on trap hit
            if (flashAlpha > 0.0f)
                DrawRectangle(0, 0, SCREEN_W, SCREEN_H, Fade(RED, flashAlpha));

            // HUD (skipped here on level 3 — drawn after canvas to avoid rotation)
            if (!(currentLevel == 2 && (state == GameState::GAMEPLAY || state == GameState::PAUSED)))
            {
            // Song progress bar (very top of screen)
            {
                float total = musicOk ? GetMusicTimeLength(music) * 1000.0f : 1.0f;
                float progress = (total > 0.0f) ? (currentTimeMs / total) : 0.0f;
                if (progress > 1.0f) progress = 1.0f;
                int barW = (int)(SCREEN_W * progress);
                DrawRectangle(0, 0, SCREEN_W, 9, { 25, 28, 45, 210 });
                if (barW > 0)
                    DrawRectangleGradientH(0, 0, barW, 9, { 70, 110, 255, 200 }, { 140, 210, 255, 240 });
                if (barW > 3)
                    DrawRectangle(barW - 3, 0, 6, 9, { 220, 235, 255, 255 });
            }

            // HUD top-left — score with pop
            {
                std::string stxt = "Счёт: " + std::to_string(score);
                float tp = (scorePopTimer > 0.0f) ? scorePopTimer / 0.18f : 0.0f;
                int   sz = (int)(57.0f * (1.0f + tp * (scorePopPerf ? 0.33f : 0.15f)));
                int   dy = (sz - 57) / 2;
                Color col = {
                    (unsigned char)(245.0f + tp * 10.0f),
                    (unsigned char)(248.0f + tp * (scorePopPerf ? -33.0f :  7.0f)),
                    (unsigned char)(248.0f + tp * (scorePopPerf ? -168.0f : 7.0f)),
                    245
                };
                drawTxt(stxt.c_str(), 33, 27 - dy, sz, col);
            }
            // Combo — small label + large number
            {
                float tp  = (comboPopTimer > 0.0f) ? comboPopTimer / 0.18f : 0.0f;
                int   nsz = (int)(78.0f * (1.0f + tp * (comboPopPerf ? 0.38f : 0.18f)));
                int   dy  = (nsz - 78) / 2;
                Color numCol = {
                    (unsigned char)(255.0f),
                    (unsigned char)(220.0f + tp * (comboPopPerf ?   0.0f : 35.0f)),
                    (unsigned char)( 80.0f + tp * (comboPopPerf ? -80.0f : 175.0f)),
                    (unsigned char)(combo > 0 ? 240 : 110)
                };
                std::string num = "x" + std::to_string(combo);
                drawTxt(num.c_str(), 30, 102 - dy, nsz, numCol);
            }
            } // end HUD skip for level 3


            // Judgment popup — compact, clean, readable
            if (judgmentTimer > 0.0f)
            {
                const char* label = "ПРОМАХ";
                Color col = { 225, 85, 85, 255 };
                float fSize = 51.0f;
                if      (lastJudgment == Judgment::PERFECT)   { label = "ИДЕАЛЬНО"; col = { 255, 215,   0, 255 }; }
                else if (lastJudgment == Judgment::GOOD)       { label = "ХОРОШО";   col = {  80, 210,  90, 255 }; }
                else if (lastJudgment == Judgment::TRAP)       { label = "ЛОВУШКА";  col = { 255,  75,  35, 255 }; }
                else if (lastJudgment == Judgment::HOLD_OK)    { label = "УДЕРЖАНО"; col = {  50, 210, 130, 255 }; }
                else if (lastJudgment == Judgment::HOLD_MISS)  { label = "СРЫВ";     col = { 190,  80, 255, 255 }; }
                fSize = 51.0f;

                float t = judgmentTimer / 1.5f;

                // Fast fade-in (0.075s), stable, fade-out from 40%
                float a01;
                if      (t > 0.95f) a01 = (1.0f - t) / 0.05f;
                else if (t > 0.38f) a01 = 1.0f;
                else                a01 = t / 0.38f;
                col.a = (unsigned char)(a01 * 255.0f);

                // Slight scale pop on appear: 1.08 -> 1.0
                float drawSize = fSize * (t > 0.95f ? (1.0f + 0.08f * (t - 0.95f) / 0.05f) : 1.0f);

                // Upward drift from just above hit line
                float yPos = HIT_Y - 57.0f - (1.5f - judgmentTimer) * 24.0f;

                if (fontOk && col.a > 0)
                {
                    if (currentLevel == 2)
                    {
                        // Hotline Miami style: centered, pink, lemon shadow, italic tilt + animation
                        float hmBaseSize = 72.0f;
                        // Scale pop on appear: 1.12 -> 1.0
                        float hmScale = (t > 0.95f) ? (1.12f - 0.12f * (t - 0.95f) / 0.05f) : 1.0f;
                        float hmSize = hmBaseSize * hmScale;
                        // Upward drift from center
                        float hmBaseY = SCREEN_H / 2.0f - 60.0f;
                        float hmY = hmBaseY - (1.5f - judgmentTimer) * 20.0f;
                        Vector2 hmPos  = { SCREEN_W / 2.0f, hmY };
                        Vector2 hmSz = MeasureTextEx(font, label, hmSize, 1.0f);
                        Vector2 hmOrig = { hmSz.x * 0.5f, hmSz.y * 0.5f };
                        float   hmRot  = -8.0f; // italic lean
                        unsigned char ha = (unsigned char)(a01 * 255.0f);
                        // Lemon shadow
                        DrawTextPro(font, label, { hmPos.x + 5.0f, hmPos.y + 5.0f },
                            hmOrig, hmRot, hmSize, 1.0f, { 255, 245, 0, ha });
                        // Pink text
                        DrawTextPro(font, label, hmPos,
                            hmOrig, hmRot, hmSize, 1.0f, { 255, 60, 180, ha });
                    }
                    else if (currentLevel == 3)
                    {
                        // Billie Jean style: bright white core, blue/violet neon glow and a quick bounce-in.
                        float mjBaseSize = 74.0f;
                        float elapsed = 1.5f - judgmentTimer;
                        float intro = elapsed / 0.22f;
                        if (intro < 0.0f) intro = 0.0f;
                        if (intro > 1.0f) intro = 1.0f;

                        float bounceScale = 1.0f + 0.30f * (1.0f - intro) * cosf(intro * 9.0f);
                        float jump = -10.0f * sinf(intro * 3.14159265f) * (1.0f - intro * 0.35f);
                        float mjSize = mjBaseSize * bounceScale;

                        float flicker = 1.0f;
                        if (elapsed < 0.20f)
                        {
                            float flickerPhase = elapsed / 0.20f;
                            flicker = 0.78f + 0.22f * (0.5f + 0.5f * sinf(flickerPhase * 30.0f));
                        }

                        unsigned char neonAlpha = (unsigned char)(a01 * flicker * 255.0f);
                        unsigned char outerGlowAlpha = (unsigned char)(a01 * flicker * 70.0f);
                        unsigned char innerGlowAlpha = (unsigned char)(a01 * flicker * 130.0f);
                        Vector2 mjSz = MeasureTextEx(font, label, mjSize, 1.0f);
                        Vector2 mjPos = { SCREEN_W / 2.0f - mjSz.x / 2.0f, HIT_Y - 72.0f + jump };

                        DrawTextEx(font, label, { mjPos.x - 6.0f, mjPos.y + 3.0f }, mjSize, 1.0f,
                            { 45, 110, 255, outerGlowAlpha });
                        DrawTextEx(font, label, { mjPos.x + 6.0f, mjPos.y + 3.0f }, mjSize, 1.0f,
                            { 95, 40, 255, outerGlowAlpha });
                        DrawTextEx(font, label, { mjPos.x - 3.0f, mjPos.y + 1.5f }, mjSize, 1.0f,
                            { 90, 150, 255, innerGlowAlpha });
                        DrawTextEx(font, label, { mjPos.x + 3.0f, mjPos.y + 1.5f }, mjSize, 1.0f,
                            { 145, 80, 255, innerGlowAlpha });
                        DrawTextEx(font, label, mjPos, mjSize, 1.0f,
                            { 255, 255, 255, neonAlpha });
                    }
                    else
                    {
                        Vector2 jSz = MeasureTextEx(font, label, drawSize, 1.0f);
                        float xPos = SCREEN_W / 2.0f - jSz.x / 2.0f;
                        DrawTextEx(font, label, { xPos + 1.0f, yPos + 2.0f }, drawSize, 1.0f,
                            { 0, 0, 0, (unsigned char)(a01 * 160.0f) });
                        DrawTextEx(font, label, { xPos, yPos }, drawSize, 1.0f, col);
                    }
                }
            }

            // Fade-out overlay (darkens screen during transition to RESULT)
            if (fadingOut && FADE_OUT_DURATION > 0.0f)
            {
                float progress = 1.0f - (fadeOutTimer / FADE_OUT_DURATION);
                if (progress < 0.0f) progress = 0.0f;
                if (progress > 1.0f) progress = 1.0f;
                unsigned char oa = (unsigned char)(progress * 220.0f);
                DrawRectangle(0, 0, SCREEN_W, SCREEN_H, { 0, 0, 0, oa });
            }


            if (state == GameState::PAUSED)
            {
                auto cTxt = [&](const char* t, float y, int sz, Color c) {
                    Vector2 s = MeasureTextEx(font, t, (float)sz, 1.0f);
                    if (fontOk) DrawTextEx(font, t, { SCREEN_W / 2.0f - s.x / 2, y }, (float)sz, 1.0f, c);
                    else DrawText(t, (int)(SCREEN_W / 2.0f - s.x / 2), (int)y, sz, c);
                };

                // Dim overlay
                DrawRectangle(0, 0, SCREEN_W, SCREEN_H, { 0, 0, 0, 150 });

                if (pauseExitModal)
                {
                    // Modal input
                    if (IsKeyPressed(KEY_LEFT) || IsKeyPressed(KEY_A)) pauseExitSelection = 0; // ДА
                    if (IsKeyPressed(KEY_RIGHT) || IsKeyPressed(KEY_D)) pauseExitSelection = 1; // НЕТ
                    if (IsKeyPressed(KEY_ENTER)) {
                        if (pauseExitSelection == 0) running = false;
                        else pauseExitModal = false;
                    }
                    if (IsKeyPressed(KEY_ESCAPE)) pauseExitModal = false;

                    // Draw Modal
                    int pw = 700, ph = 350;
                    int px = SCREEN_W / 2 - pw / 2, py = SCREEN_H / 2 - ph / 2;
                    DrawRectangle(px, py, pw, ph, { 20, 22, 35, 240 });
                    DrawRectangleLines(px, py, pw, ph, { 80, 80, 140, 200 });

                    cTxt("Вы действительно хотите выйти?", (float)py + 80, 48, WHITE);

                    const char* btnYes = "ДА";
                    const char* btnNo = "НЕТ";
                    
                    Vector2 szYes = MeasureTextEx(font, btnYes, 48.0f, 1.0f);
                    Vector2 szNo = MeasureTextEx(font, btnNo, 48.0f, 1.0f);
                    
                    Rectangle recYes = { (float)px + 150, (float)py + 200, szYes.x + 60, 70 };
                    Rectangle recNo = { (float)px + pw - 150 - szNo.x - 60, (float)py + 200, szNo.x + 60, 70 };

                    Vector2 mouse = GetMousePosition();
                    if (CheckCollisionPointRec(mouse, recYes)) {
                        pauseExitSelection = 0;
                        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) running = false;
                    }
                    if (CheckCollisionPointRec(mouse, recNo)) {
                        pauseExitSelection = 1;
                        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) pauseExitModal = false;
                    }

                    DrawRectangleRec(recYes, pauseExitSelection == 0 ? Color{255,255,255,40} : BLANK);
                    DrawRectangleLinesEx(recYes, 2.0f, pauseExitSelection == 0 ? WHITE : GRAY);
                    if (fontOk) DrawTextEx(font, btnYes, { recYes.x + 30, recYes.y + 11 }, 48.0f, 1.0f, pauseExitSelection == 0 ? WHITE : GRAY);

                    DrawRectangleRec(recNo, pauseExitSelection == 1 ? Color{255,255,255,40} : BLANK);
                    DrawRectangleLinesEx(recNo, 2.0f, pauseExitSelection == 1 ? WHITE : GRAY);
                    if (fontOk) DrawTextEx(font, btnNo, { recNo.x + 30, recNo.y + 11 }, 48.0f, 1.0f, pauseExitSelection == 1 ? WHITE : GRAY);
                }
                else
                {
                    // Panel
                    int pw = 630, ph = 390;
                    int px = SCREEN_W / 2 - pw / 2, py = SCREEN_H / 2 - ph / 2;
                    DrawRectangle(px, py, pw, ph, { 20, 22, 35, 240 });
                    DrawRectangleLines(px, py, pw, ph, { 80, 80, 140, 200 });

                    // Title
                    Vector2 pSz = MeasureTextEx(font, "Пауза", 78.0f, 1.0f);
                    if (fontOk) DrawTextEx(font, "Пауза", { SCREEN_W / 2.0f - pSz.x / 2, (float)py + 30 }, 78.0f, 1.0f, WHITE);
                    else DrawText("Пауза", (int)(SCREEN_W / 2.0f - pSz.x / 2), py + 30, 78, WHITE);

                    // Input — skip all key checks on the frame we just entered pause
                    if (justPaused) { justPaused = false; }
                    else
                    {
                        if (IsKeyPressed(KEY_ESCAPE))
                        {
                            if (musicOk) ResumeMusicStream(music);
                            state = GameState::GAMEPLAY;
                        }
                        if (IsKeyPressed(KEY_UP)   || IsKeyPressed(KEY_W)) pauseSelection = (pauseSelection + 2) % 3;
                        if (IsKeyPressed(KEY_DOWN) || IsKeyPressed(KEY_S)) pauseSelection = (pauseSelection + 1) % 3;
                        if (IsKeyPressed(KEY_ENTER))
                        {
                            if (pauseSelection == 0)
                            {
                                if (musicOk) ResumeMusicStream(music);
                                state = GameState::GAMEPLAY;
                            }
                            else if (pauseSelection == 1)
                            {
                                if (musicOk) StopMusicStream(music);
                                state = GameState::MENU;
                            }
                            else { pauseExitModal = true; pauseExitSelection = 1; }
                        }
                    } // end else (not justPaused)

                    // Menu items
                    const char* pItems[3] = { "Продолжить", "Главное меню", "Выйти из игры" };
                    Vector2 mouse = GetMousePosition();
                    
                    for (int i = 0; i < 3; i++)
                    {
                        float by = (float)(py + 132 + i * 75);
                        Rectangle itemRec = { (float)px + 30, by - 6, (float)pw - 60, 60 };
                        
                        if (CheckCollisionPointRec(mouse, itemRec) && !justPaused) {
                            pauseSelection = i;
                            if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                                if (i == 0) {
                                    if (musicOk) ResumeMusicStream(music);
                                    state = GameState::GAMEPLAY;
                                } else if (i == 1) {
                                    if (musicOk) StopMusicStream(music);
                                    state = GameState::MENU;
                                } else {
                                    pauseExitModal = true; pauseExitSelection = 1;
                                }
                            }
                        }

                        Color c = (i == pauseSelection)
                            ? Color{ 255, 230, 80, 255 } : Color{ 180, 185, 195, 255 };
                        Vector2 sz = MeasureTextEx(font, pItems[i], 42.0f, 1.0f);
                        
                        if (i == pauseSelection)
                            DrawRectangleRec(itemRec, { 255, 255, 255, 20 });
                            
                        if (fontOk) DrawTextEx(font, pItems[i], { SCREEN_W / 2.0f - sz.x / 2, by }, 42.0f, 1.0f, c);
                        else DrawText(pItems[i], (int)(SCREEN_W / 2.0f - sz.x / 2), (int)by, 42, c);
                    }
                }
            }
        }
        // ================================================================
        // RESULT
        // ================================================================
        else if (state == GameState::RESULT)
        {
            if (IsKeyPressed(KEY_R))      resetGame(currentLevel);
            if (IsKeyPressed(KEY_ESCAPE)) state = GameState::MENU;

            auto cTxt = [&](const char* t, float y, int sz, Color c)
                {
                    Vector2 s = MeasureTextEx(font, t, (float)sz, 1.0f);
                    if (fontOk) DrawTextEx(font, t, { SCREEN_W / 2.0f - s.x / 2, y }, (float)sz, 1.0f, c);
                    else DrawText(t, (int)(SCREEN_W / 2.0f - s.x / 2), (int)y, sz, c);
                };
            auto drawRightTxt = [&](const char* t, int rightX, int y, int sz, Color c)
                {
                    Vector2 s = MeasureTextEx(font, t, (float)sz, 1.0f);
                    if (fontOk) DrawTextEx(font, t, { rightX - s.x, (float)y }, (float)sz, 1.0f, c);
                    else DrawText(t, rightX - (int)s.x, y, sz, c);
                };
            auto drawCenteredInRect = [&](const char* t, Rectangle rec, int sz, Color c)
                {
                    Vector2 s = MeasureTextEx(font, t, (float)sz, 1.0f);
                    float tx = rec.x + (rec.width - s.x) * 0.5f;
                    float ty = rec.y + (rec.height - s.y) * 0.5f;
                    if (fontOk) DrawTextEx(font, t, { tx, ty }, (float)sz, 1.0f, c);
                    else DrawText(t, (int)tx, (int)ty, sz, c);
                };

            // Accuracy calculation
            int totalJudged = perfectCount + goodCount + missCount + trapErrors + holdOkCount + holdMissCount;
            float accuracy  = (totalJudged > 0)
                ? (float)(perfectCount * 100 + goodCount * 50 + holdOkCount * 100) / (float)(totalJudged * 100) * 100.0f
                : 0.0f;
            char accBuf[16];
            snprintf(accBuf, sizeof(accBuf), "%.2f%%", accuracy);
            Color accCol = (accuracy >= 95.0f) ? Color{ 255, 215,   0, 255 } :
                           (accuracy >= 80.0f) ? Color{ 100, 230, 120, 255 } :
                           (accuracy >= 60.0f) ? Color{  80, 200, 220, 255 } :
                                                 Color{ 220, 100, 100, 255 };
            const char* rankLabel = "D";
            Color rankCol = { 220, 100, 100, 255 };
            if      (accuracy >= 98.0f) { rankLabel = "S"; rankCol = { 255, 225, 120, 255 }; }
            else if (accuracy >= 92.0f) { rankLabel = "A"; rankCol = { 120, 230, 150, 255 }; }
            else if (accuracy >= 82.0f) { rankLabel = "B"; rankCol = { 100, 205, 255, 255 }; }
            else if (accuracy >= 70.0f) { rankLabel = "C"; rankCol = { 180, 130, 255, 255 }; }

            float resultPulse = 0.5f + 0.5f * sinf(swayTime * 2.2f);
            std::string scoreStr = std::to_string(score);
            std::string comboStr = std::to_string(maxCombo);
            std::string levelText = std::string(LEVEL_DEFS[currentLevel].name) + "  /  " +
                ((levelChartOk[currentLevel] && !levelTemplates[currentLevel].difficulty.empty())
                    ? levelTemplates[currentLevel].difficulty
                    : LEVEL_DEFS[currentLevel].difficulty);
            std::string recordText = "Рекорд уровня: " + std::to_string(highScores[currentLevel]);

            DrawRectangleGradientV(0, 0, SCREEN_W, SCREEN_H, { 4, 6, 16, 255 }, { 2, 2, 8, 255 });
            DrawRectangleGradientH(0, 0, SCREEN_W, SCREEN_H, { 20, 25, 50, 20 }, { 0, 0, 0, 0 });

            Vector2 rSz = MeasureTextEx(font, "РЕЗУЛЬТАТ", 108.0f, 2.0f);
            if (fontOk) DrawTextEx(font, "РЕЗУЛЬТАТ",
                { SCREEN_W / 2.0f - rSz.x / 2, 56 }, 108.0f, 2.0f, WHITE);
            DrawLineEx({ SCREEN_W / 2.0f - 420, 186 }, { SCREEN_W / 2.0f + 420, 186 },
                3.0f, { 85, 95, 150, 180 });
            cTxt(levelText.c_str(), 205, 28, { 155, 165, 220, 255 });

            Rectangle leftCard = { 475, 252, 470, 188 };
            Rectangle rightCard = { 975, 252, 470, 188 };
            Rectangle statsCard = { 475, 470, 970, 312 };
            DrawRectangleRec(leftCard, { 8, 10, 18, 215 });
            DrawRectangleLinesEx(leftCard, 2.0f, { 70, 80, 135, 200 });
            DrawRectangleRec(rightCard, { 8, 10, 18, 215 });
            DrawRectangleLinesEx(rightCard, 2.0f, { 70, 80, 135, 200 });
            DrawRectangleRec(statsCard, { 5, 7, 14, 195 });
            DrawRectangleLinesEx(statsCard, 2.0f, { 60, 70, 120, 180 });

            DrawRectangleGradientH((int)leftCard.x, (int)leftCard.y, (int)leftCard.width, 8,
                { 90, 140, 255, 180 }, { 30, 60, 120, 0 });
            DrawRectangleGradientH((int)rightCard.x, (int)rightCard.y, (int)rightCard.width, 8,
                { rankCol.r, rankCol.g, rankCol.b, 190 }, { rankCol.r, rankCol.g, rankCol.b, 0 });
            DrawRectangleGradientH((int)statsCard.x, (int)statsCard.y, (int)statsCard.width, 8,
                { 100, 120, 255, 120 }, { 20, 30, 70, 0 });

            drawTxt("СЧЕТ", (int)leftCard.x + 28, (int)leftCard.y + 22, 24, { 150, 165, 220, 255 });
            drawTxt(scoreStr.c_str(), (int)leftCard.x + 28, (int)leftCard.y + 54, 76, WHITE);
            drawTxt(recordText.c_str(), (int)leftCard.x + 28, (int)leftCard.y + 138, 24, { 150, 165, 220, 255 });

            drawTxt("ТОЧНОСТЬ", (int)rightCard.x + 28, (int)rightCard.y + 22, 24, { 150, 165, 220, 255 });
            drawTxt(accBuf, (int)rightCard.x + 28, (int)rightCard.y + 52, 64, accCol);
            Vector2 rankCenter = { rightCard.x + rightCard.width - 82.0f, rightCard.y + 92.0f };
            DrawCircle((int)rankCenter.x, (int)rankCenter.y, (int)(48 + resultPulse * 6.0f),
                { rankCol.r, rankCol.g, rankCol.b, (unsigned char)(28 + resultPulse * 24.0f) });
            Rectangle rankRect = { rankCenter.x - 60.0f, rankCenter.y - 60.0f, 120.0f, 120.0f };
            drawCenteredInRect(rankLabel, rankRect, 84, rankCol);

            int sy = (int)statsCard.y + 28;
            int sx = (int)statsCard.x + 34;
            int vx = sx + 430;
            int sx2 = (int)statsCard.x + 540;
            int vx2 = sx2 + 320;
            drawTxt("Серия и базовая статистика", sx, sy - 2, 26, { 210, 220, 255, 255 });
            drawTxt("Детали прохождения", sx2, sy - 2, 26, { 210, 220, 255, 255 });
            DrawLineEx({ (float)sx, (float)(sy + 34) }, { (float)(statsCard.x + statsCard.width - 34), (float)(sy + 34) }, 1.5f, { 55, 65, 110, 160 });
            sy += 58;
            drawTxt("Макс. комбо", sx,  sy, 34, {255,200, 50,255}); drawRightTxt(comboStr.c_str(),                    vx,  sy, 34, {255,200, 50,255}); sy += 50;
            drawTxt("Идеально",   sx,  sy, 34, {255,215,  0,255}); drawRightTxt(std::to_string(perfectCount).c_str(), vx,  sy, 34, {255,215,  0,255}); sy += 48;
            drawTxt("Хорошо",     sx,  sy, 34, {100,220,100,255}); drawRightTxt(std::to_string(goodCount).c_str(),    vx,  sy, 34, {100,220,100,255}); sy += 48;
            drawTxt("Промах",     sx,  sy, 34, {220, 80, 80,255}); drawRightTxt(std::to_string(missCount).c_str(),    vx,  sy, 34, {220, 80, 80,255});

            int sy2 = (int)statsCard.y + 86;
            drawTxt("Ловушки",        sx2, sy2, 34, {255,100, 80,255}); drawRightTxt(std::to_string(trapErrors).c_str(),    vx2, sy2, 34, {255,100, 80,255}); sy2 += 48;
            drawTxt("Удержано",       sx2, sy2, 34, { 60,220,120,255}); drawRightTxt(std::to_string(holdOkCount).c_str(),   vx2, sy2, 34, { 60,220,120,255}); sy2 += 48;
            drawTxt("Срыв удержания", sx2, sy2, 34, {200,100,255,255}); drawRightTxt(std::to_string(holdMissCount).c_str(), vx2, sy2, 34, {200,100,255,255});

            Rectangle retryBtn = { 620, 828, 300, 66 };
            Rectangle menuBtn = { 1000, 828, 300, 66 };
            Vector2 mouse = GetMousePosition();
            bool retryHover = CheckCollisionPointRec(mouse, retryBtn);
            bool menuHover = CheckCollisionPointRec(mouse, menuBtn);
            if (retryHover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) resetGame(currentLevel);
            if (menuHover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) state = GameState::MENU;
            DrawRectangleRec(retryBtn, retryHover ? Color{ 80, 100, 220, 35 } : Color{ 20, 25, 45, 20 });
            DrawRectangleLinesEx(retryBtn, 2.0f, retryHover ? Color{ 180, 200, 255, 255 } : Color{ 85, 100, 160, 180 });
            DrawRectangleRec(menuBtn, menuHover ? Color{ 80, 100, 220, 26 } : Color{ 20, 25, 45, 18 });
            DrawRectangleLinesEx(menuBtn, 2.0f, menuHover ? Color{ 170, 190, 255, 230 } : Color{ 70, 85, 145, 170 });
            drawCenteredInRect("R  -  НАЧАТЬ ЗАНОВО", retryBtn, 30, retryHover ? WHITE : Color{ 210, 220, 255, 255 });
            drawCenteredInRect("ESC  -  В МЕНЮ", menuBtn, 30, menuHover ? WHITE : Color{ 185, 195, 225, 255 });
        }

        EndTextureMode();

        // Scale virtual 1920×1080 canvas to actual screen
        float sw = (float)GetScreenWidth();
        float sh = (float)GetScreenHeight();
        float sc = (sw / SCREEN_W < sh / SCREEN_H) ? sw / SCREEN_W : sh / SCREEN_H;
        float ox = (sw - SCREEN_W * sc) * 0.5f;
        float oy = (sh - SCREEN_H * sc) * 0.5f;
        float dw = (float)SCREEN_W * sc;
        float dh = (float)SCREEN_H * sc;

        // Hotline Miami effects (level 3 gameplay only)
        bool hmActive = (currentLevel == 2) &&
                        (state == GameState::GAMEPLAY || state == GameState::PAUSED);

        // Camera swim: figure-8 offset + clamped rotation
        float swayRot  = 0.0f;
        float swayOffX = 0.0f, swayOffY = 0.0f;
        if (hmActive)
        {
            // Rotation: two incommensurable freqs, clamped to ±1.2°
            swayRot = sinf(swayTime * 0.53f) * 1.0f + sinf(swayTime * 1.27f) * 0.22f;
            if (swayRot >  1.2f) swayRot =  1.2f;
            if (swayRot < -1.2f) swayRot = -1.2f;

            // Figure-8 swim: freq ratio ~1:2 (Lissajous) + slow irrational drift
            swayOffX = sinf(swayTime * 0.41f) * 7.0f + sinf(swayTime * 0.19f) * 3.0f;
            swayOffY = sinf(swayTime * 0.82f) * 5.0f + sinf(swayTime * 0.29f) * 2.5f;
        }

        BeginDrawing();
        ClearBackground(BLACK);
        // Fill screen with matching game gradient so rotated canvas edges are invisible
        if (hmActive)
            DrawRectangle(0, 0, (int)sw, (int)sh, currentBgColor);

        Rectangle srcRect = { 0.0f, 0.0f, (float)SCREEN_W, -(float)SCREEN_H };

        float destX = hmActive ? ox + dw * 0.5f + swayOffX : ox;
        float destY = hmActive ? oy + dh * 0.5f + swayOffY : oy;
        Vector2 orig = { hmActive ? dw * 0.5f : 0.0f, hmActive ? dh * 0.5f : 0.0f };

        if (hmActive)
        {
            DrawTexturePro(canvas.texture, srcRect, { destX, destY, dw, dh }, orig, swayRot, WHITE);

            // Chromatic aberration: additive red/blue fringes, intensity driven by smoothCA
            if (smoothCA > 0.01f)
            {
                float ab = 10.0f * smoothCA;
                unsigned char caA = (unsigned char)(smoothCA * 90.0f);
                BeginBlendMode(BLEND_ADDITIVE);
                DrawTexturePro(canvas.texture, srcRect, { destX - ab, destY, dw, dh }, orig, swayRot, { 255,   0,   0, caA });
                DrawTexturePro(canvas.texture, srcRect, { destX + ab, destY, dw, dh }, orig, swayRot, {   0,   0, 255, caA });
                EndBlendMode();
            }

            // HUD overlay — drawn without rotation so it stays fixed
            {
                float total = musicOk ? GetMusicTimeLength(music) * 1000.0f : 1.0f;
                float progress = (total > 0.0f) ? (hudCurrentTimeMs / total) : 0.0f;
                if (progress > 1.0f) progress = 1.0f;
                int barW = (int)(SCREEN_W * progress);
                DrawRectangle(0, 0, SCREEN_W, 9, { 25, 28, 45, 210 });
                if (barW > 0)
                    DrawRectangleGradientH(0, 0, barW, 9, { 70, 110, 255, 200 }, { 140, 210, 255, 240 });
                if (barW > 3)
                    DrawRectangle(barW - 3, 0, 6, 9, { 220, 235, 255, 255 });
            }
            {
                std::string stxt = "Счёт: " + std::to_string(score);
                float tp = (scorePopTimer > 0.0f) ? scorePopTimer / 0.18f : 0.0f;
                int   sz = (int)(57.0f * (1.0f + tp * (scorePopPerf ? 0.33f : 0.15f)));
                int   dy = (sz - 57) / 2;
                Color col = {
                    (unsigned char)(245.0f + tp * 10.0f),
                    (unsigned char)(248.0f + tp * (scorePopPerf ? -33.0f :  7.0f)),
                    (unsigned char)(248.0f + tp * (scorePopPerf ? -168.0f : 7.0f)),
                    245
                };
                if (fontOk) DrawTextEx(font, stxt.c_str(), { 33.0f, (float)(27 - dy) }, (float)sz, 1.0f, col);
            }
            {
                float tp  = (comboPopTimer > 0.0f) ? comboPopTimer / 0.18f : 0.0f;
                int   nsz = (int)(78.0f * (1.0f + tp * (comboPopPerf ? 0.38f : 0.18f)));
                int   dy  = (nsz - 78) / 2;
                Color numCol = {
                    255,
                    (unsigned char)(220.0f + tp * (comboPopPerf ?   0.0f : 35.0f)),
                    (unsigned char)( 80.0f + tp * (comboPopPerf ? -80.0f : 175.0f)),
                    (unsigned char)(combo > 0 ? 240 : 110)
                };
                std::string num = "x" + std::to_string(combo);
                if (fontOk) DrawTextEx(font, num.c_str(), { 30.0f, (float)(102 - dy) }, (float)nsz, 1.0f, numCol);
            }

            // Sync fade-out overlay with canvas (covers HUD too)
            if (fadingOut && FADE_OUT_DURATION > 0.0f)
            {
                float progress = 1.0f - (fadeOutTimer / FADE_OUT_DURATION);
                if (progress < 0.0f) progress = 0.0f;
                if (progress > 1.0f) progress = 1.0f;
                DrawRectangle(0, 0, (int)sw, (int)sh, { 0, 0, 0, (unsigned char)(progress * 220.0f) });
            }
        }
        else
        {
            DrawTexturePro(canvas.texture, srcRect, { destX, destY, dw, dh }, orig, swayRot, WHITE);
        }

        EndDrawing();
    }

    // --- Cleanup ---
    UnloadRenderTexture(canvas);
    if (musicOk) UnloadMusicStream(music);
    if (fontOk)  UnloadFont(font);
    CloseAudioDevice();
    CloseWindow();

    return 0;
}
