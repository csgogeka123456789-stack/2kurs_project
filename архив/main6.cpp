#include "raylib.h"
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------
constexpr float NOTE_SPEED = 0.5f;    // pixels per millisecond
constexpr float HIT_Y = 600.0f;  // y-coordinate of the hit line
constexpr float NOTE_RADIUS = 18.0f;
constexpr float LANE_WIDTH = 96.0f;

// Lane centers (3 vertical lanes, centered on screen)
const float LANE_CENTERS[3] = { 480.0f, 640.0f, 800.0f };
const Color LANE_COLORS[3] = {
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
constexpr int LEVEL_COUNT = 3;
const LevelDef LEVEL_DEFS[LEVEL_COUNT] = {
    { "Уровень 1", "assets/track.ogg",  "assets/example.json",  "Лёгкий",  {  80, 220, 100, 255 } },
    { "Уровень 2", "assets/track2.ogg", "assets/example2.json", "Средний", { 255, 200,  40, 255 } },
    { "Уровень 3", "assets/track3.ogg", "assets/example3.json", "Сложный", { 230,  70,  70, 255 } },
};

// Hit windows calibrated to NOTE_SPEED=0.5 px/ms and NOTE_RADIUS=18px:
//   PERFECT 45ms → note 22.5px from line → bottom edge 4.5px above (visually touching)
//   GOOD    90ms → note 45px from line  → bottom edge 27px above  (clearly approaching)
constexpr int HIT_WINDOW_PERFECT = 45;   // ±45 ms
constexpr int HIT_WINDOW_GOOD = 90;   // ±90 ms
constexpr int HIT_WINDOW_MISS = 200;  // ±200 ms — auto-miss / ghost-tap boundary

// ---------------------------------------------------------------------------
// Parse chart  (no external libraries — plain std::string search)
// ---------------------------------------------------------------------------

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

bool parseChart(const std::string& path, int& outBpm, std::vector<Note>& outNotes)
{
    std::ifstream f(path);
    if (!f.is_open()) return false;

    std::ostringstream ss;
    ss << f.rdbuf();
    const std::string src = ss.str();
    if (src.empty()) return false;

    outBpm = jsonInt(src, "bpm");

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

        outNotes.push_back(note);
        pos = objEnd + 1;
    }

    return outBpm > 0 && !outNotes.empty();
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
int main(void)
{
    const int SCREEN_W = 1280;
    const int SCREEN_H = 720;

    SetConfigFlags(FLAG_FULLSCREEN_MODE);
    InitWindow(SCREEN_W, SCREEN_H, "Синхрон");
    InitAudioDevice();
    SetTargetFPS(60);
    SetExitKey(KEY_NULL);

    RenderTexture2D canvas = LoadRenderTexture(SCREEN_W, SCREEN_H);
    SetTextureFilter(canvas.texture, TEXTURE_FILTER_BILINEAR);

    // --- Load assets ---

    // Font with ASCII + full Cyrillic block
    const int CP_COUNT = 96 + 256 + 1;
    int codepoints[CP_COUNT];
    for (int i = 0; i < 96; i++) codepoints[i] = 32 + i;
    for (int i = 0; i < 256; i++) codepoints[96 + i] = 0x0400 + i;
    codepoints[96 + 256] = 0x2014;  // em dash —
    Font font = LoadFontEx("assets/font.ttf", 64, codepoints, CP_COUNT);
    SetTextureFilter(font.texture, TEXTURE_FILTER_BILINEAR);

    bool fontOk = (font.texture.id > 0);

    // --- Pre-parse all level charts ---
    std::vector<std::vector<Note>> levelTemplates(LEVEL_COUNT);
    bool levelChartOk[LEVEL_COUNT] = {};
    { int tmpBpm = 0;
      for (int i = 0; i < LEVEL_COUNT; i++)
          levelChartOk[i] = parseChart(LEVEL_DEFS[i].chartPath, tmpBpm, levelTemplates[i]); }

    // Load music for level 0 initially
    int               currentLevel = 0;
    Music             music        = LoadMusicStream(LEVEL_DEFS[0].musicPath);
    bool              musicOk      = (music.frameCount > 0);
    std::vector<Note> notes        = levelTemplates[0];
    bool              chartOk      = levelChartOk[0];

    // --- Game state ---
    GameState state = GameState::MENU;
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
    int      selectedLevel = 0;
    float    flashAlpha = 0.0f;
    float    shakeTime = 0.0f;
    int      shakeOffsetX = 0;
    int      shakeOffsetY = 0;
    float    hitEffect[3]        = { 0.0f, 0.0f, 0.0f };
    bool     hitEffectPerfect[3] = { false, false, false };
    float    comboPopTimer = 0.0f;
    float    scorePopTimer = 0.0f;
    bool     comboPopPerf  = false;
    bool     scorePopPerf  = false;

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
            notes = levelTemplates[levelIdx];
            score = perfectCount = goodCount = missCount = 0;
            trapErrors = holdOkCount = holdMissCount = combo = maxCombo = 0;
            lastJudgment = Judgment::NONE;
            judgmentTimer = 0.0f;
            if (musicOk) { StopMusicStream(music); PlayMusicStream(music); }
            state = GameState::GAMEPLAY;
        };

    bool running = true;
    while (!WindowShouldClose() && running)
    {
        float dt = GetFrameTime();

        BeginTextureMode(canvas);
        ClearBackground({ 12, 12, 22, 255 });

        // ================================================================
        // MENU
        // ================================================================
        if (state == GameState::MENU)
        {
            if (IsKeyPressed(KEY_LEFT)  || IsKeyPressed(KEY_A))
                selectedLevel = (selectedLevel + LEVEL_COUNT - 1) % LEVEL_COUNT;
            if (IsKeyPressed(KEY_RIGHT) || IsKeyPressed(KEY_D))
                selectedLevel = (selectedLevel + 1) % LEVEL_COUNT;
            if (IsKeyPressed(KEY_ENTER)) resetGame(selectedLevel);
            if (IsKeyPressed(KEY_ESCAPE)) running = false;

            auto cTxt = [&](const char* t, float y, int sz, Color c)
                {
                    Vector2 s = MeasureTextEx(font, t, (float)sz, 1.0f);
                    DrawTextEx(font, t, { SCREEN_W / 2.0f - s.x / 2, y }, (float)sz, 1.0f, c);
                };

            Vector2 tSz = MeasureTextEx(font, "СИНХРОН", 88.0f, 2.0f);
            DrawTextEx(font, "СИНХРОН",
                { SCREEN_W / 2.0f - tSz.x / 2, 130 }, 88.0f, 2.0f, WHITE);
            DrawLineEx({ SCREEN_W / 2.0f - 230, 248 }, { SCREEN_W / 2.0f + 230, 248 },
                2.0f, { 80, 80, 130, 200 });

            // Level selector
            const char* lvlName = LEVEL_DEFS[selectedLevel].name;
            Vector2 lvlSz = MeasureTextEx(font, lvlName, 32.0f, 1.0f);
            Vector2 arrSz = MeasureTextEx(font, "<", 32.0f, 1.0f);
            float lvlY = 272.0f;
            DrawTextEx(font, "<",     { SCREEN_W / 2.0f - lvlSz.x / 2 - arrSz.x - 20, lvlY }, 32.0f, 1.0f, { 160, 160, 220, 200 });
            DrawTextEx(font, lvlName, { SCREEN_W / 2.0f - lvlSz.x / 2,                lvlY }, 32.0f, 1.0f, { 220, 225, 255, 255 });
            DrawTextEx(font, ">",     { SCREEN_W / 2.0f + lvlSz.x / 2 + 20,           lvlY }, 32.0f, 1.0f, { 160, 160, 220, 200 });
            // Difficulty label
            const char* diff = LEVEL_DEFS[selectedLevel].difficulty;
            Vector2 diffSz = MeasureTextEx(font, diff, 22.0f, 1.0f);
            DrawTextEx(font, diff, { SCREEN_W / 2.0f - diffSz.x / 2, lvlY + 38.0f }, 22.0f, 1.0f, LEVEL_DEFS[selectedLevel].diffColor);

            cTxt("ENTER — начать игру",                 340, 28, { 200, 200, 255, 255 });
            cTxt("A / S / D — нажимать ноты",           392, 22, { 140, 140, 190, 255 });
            cTxt("Красные ноты — ЛОВУШКИ, не нажимать!", 420, 22, { 220,  80,  80, 255 });
            cTxt("Золотые ноты — удерживать до конца",   448, 22, { 255, 200,  40, 255 });
            cTxt("ESC — выйти из игры",                  490, 22, { 140, 140, 180, 255 });

            if (!levelChartOk[selectedLevel])
                cTxt(("ОШИБКА: " + std::string(LEVEL_DEFS[selectedLevel].chartPath) + " не найден").c_str(), 536, 20, RED);
            if (!std::ifstream(LEVEL_DEFS[selectedLevel].musicPath).good())
                cTxt(("ОШИБКА: " + std::string(LEVEL_DEFS[selectedLevel].musicPath) + " не найден").c_str(), 560, 20, RED);
        }
        // ================================================================
        // GAMEPLAY
        // ================================================================
        else if (state == GameState::GAMEPLAY || state == GameState::PAUSED)
        {
            // --- Shared time ---
            if (state == GameState::GAMEPLAY && musicOk) UpdateMusicStream(music);
            float currentTimeMs = musicOk ? GetMusicTimePlayed(music) * 1000.0f : 0.0f;

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

                // Auto-miss: normal/hold-idle notes that passed start window
                // Trap notes that passed → silent deactivation
                for (auto& note : notes)
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
                for (auto& note : notes)
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
                        score += 150;
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
                            flashAlpha = 0.5f;
                            shakeTime = 0.3f;
                        }
                        else if (notes[bestIdx].type == NoteType::HOLD &&
                            notes[bestIdx].holdState == HoldState::IDLE)
                        {
                            if (absDiff <= (float)HIT_WINDOW_GOOD)
                            {
                                notes[bestIdx].holdState = HoldState::HOLDING;
                                if (absDiff <= (float)HIT_WINDOW_PERFECT) { score += 100; perfectCount++; lastJudgment = Judgment::PERFECT; hitEffect[lane-1] = 0.15f; hitEffectPerfect[lane-1] = true; comboPopTimer = scorePopTimer = 0.18f; comboPopPerf = scorePopPerf = true; }
                                else { score += 50; goodCount++; lastJudgment = Judgment::GOOD; hitEffect[lane-1] = 0.12f; hitEffectPerfect[lane-1] = false; comboPopTimer = scorePopTimer = 0.18f; comboPopPerf = scorePopPerf = false; }
                                combo++;
                                if (combo > maxCombo) maxCombo = combo;
                                judgmentTimer = 1.0f;
                            }
                        }
                        else if (absDiff <= (float)HIT_WINDOW_PERFECT)
                        {
                            notes[bestIdx].active = false;
                            score += 100; perfectCount++;
                            combo++; if (combo > maxCombo) maxCombo = combo;
                            lastJudgment = Judgment::PERFECT;
                            judgmentTimer = 1.5f;
                            hitEffect[lane-1] = 0.15f; hitEffectPerfect[lane-1] = true;
                            comboPopTimer = scorePopTimer = 0.18f; comboPopPerf = scorePopPerf = true;
                        }
                        else if (absDiff <= (float)HIT_WINDOW_GOOD)
                        {
                            notes[bestIdx].active = false;
                            score += 50; goodCount++;
                            combo++; if (combo > maxCombo) maxCombo = combo;
                            lastJudgment = Judgment::GOOD;
                            judgmentTimer = 1.5f;
                            hitEffect[lane-1] = 0.12f; hitEffectPerfect[lane-1] = false;
                            comboPopTimer = scorePopTimer = 0.18f; comboPopPerf = scorePopPerf = false;
                        }
                        // else: note exists but is outside GOOD window — press ignored, note keeps falling
                    };

                if (IsKeyPressed(KEY_A)) processLane(1);
                if (IsKeyPressed(KEY_S)) processLane(2);
                if (IsKeyPressed(KEY_D)) processLane(3);

                if (judgmentTimer > 0.0f) judgmentTimer -= dt;
                for (int i = 0; i < 3; i++) if (hitEffect[i] > 0.0f) { hitEffect[i] -= dt; if (hitEffect[i] < 0.0f) hitEffect[i] = 0.0f; }
                if (comboPopTimer > 0.0f) { comboPopTimer -= dt; if (comboPopTimer < 0.0f) comboPopTimer = 0.0f; }
                if (scorePopTimer > 0.0f) { scorePopTimer -= dt; if (scorePopTimer < 0.0f) scorePopTimer = 0.0f; }
                if (flashAlpha > 0.0f) { flashAlpha -= dt * 1.5f; if (flashAlpha < 0.0f) flashAlpha = 0.0f; }
                if (shakeTime > 0.0f) { shakeTime -= dt; shakeOffsetX = GetRandomValue(-5, 5); shakeOffsetY = GetRandomValue(-5, 5); }
                else { shakeOffsetX = 0; shakeOffsetY = 0; }

                // Song end: all notes gone or music stopped
                bool allDone = true;
                for (const auto& n : notes)
                    if (n.active && !n.scrollingOff && !(n.type == NoteType::HOLD && n.holdState == HoldState::FAILED))
                    { allDone = false; break; }
                if (state == GameState::GAMEPLAY &&
                    (allDone || (musicOk && !IsMusicStreamPlaying(music) && currentTimeMs > 500.0f)))
                    state = GameState::RESULT;
            } // end if (state == GAMEPLAY) update block

            // --- Draw gameplay ---
            // Background gradient + subtle light spots
            DrawRectangleGradientV(0, 0, SCREEN_W, SCREEN_H,
                { 18, 20, 30, 255 }, { 8, 8, 14, 255 });

            // Vertical lane columns — layered for subtle depth
            for (int i = 0; i < 3; i++)
            {
                float cx = LANE_CENTERS[i] + (float)shakeOffsetX;
                int   lx = (int)(cx - LANE_WIDTH * 0.5f);
                int   lw = (int)LANE_WIDTH;
                Color lc = LANE_COLORS[i];

                // Base: vertical gradient — darker at top, slightly warmer at bottom
                DrawRectangleGradientV(lx, 0, lw, SCREEN_H, { 20, 24, 34, 188 }, { 30, 36, 50, 200 });

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

            // Hit line glow
            DrawRectangle(0, (int)(HIT_Y - 2.0f + shakeOffsetY), SCREEN_W, 4, { 180, 180, 220, 40 });

            // Hit targets (colored rings at HIT_Y)
            const char* laneKeys[3] = { "A", "S", "D" };
            for (int i = 0; i < 3; i++)
            {
                float cx = LANE_CENTERS[i] + (float)shakeOffsetX;
                float hy = HIT_Y + (float)shakeOffsetY;
                Color lc = LANE_COLORS[i];
                DrawCircle((int)cx, (int)hy, 46, { lc.r, lc.g, lc.b, 35 });
                DrawRing({ cx, hy }, 31.0f, 39.0f, 0.0f, 360.0f, 72, lc);
                DrawCircle((int)cx, (int)hy, 28, { 30, 35, 38, 235 });
                DrawRing({ cx, hy }, 27.0f, 29.0f, 0.0f, 360.0f, 72, { 230, 235, 235, 55 });
                drawTxt(laneKeys[i], (int)(cx - 8.0f), (int)(hy + 46.0f), 24, { 235, 238, 238, 240 });
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
                float expand = perf ? 40.0f : 24.0f;
                float ringR  = 44.0f + (1.0f - t) * expand;    // ring expands outward
                unsigned char ringA  = (unsigned char)(t * (perf ? 220 : 140));
                unsigned char flashA = (unsigned char)(t * t * (perf ? 80 : 45));
                // Expanding ring
                DrawRing({ cx, hy }, ringR - 3.5f, ringR + 3.5f, 0.0f, 360.0f, 48,
                    { lc.r, lc.g, lc.b, ringA });
                // Soft glow fill
                DrawCircle((int)cx, (int)hy, (int)ringR, { lc.r, lc.g, lc.b, flashA });
                // Perfect: extra white center burst
                if (perf)
                {
                    unsigned char burstA = (unsigned char)(t * t * 120);
                    DrawCircle((int)cx, (int)hy, (int)(30.0f + (1.0f - t) * 12.0f),
                        { 255, 255, 255, burstA });
                }
            }

            // Notes (fall top → bottom)
            for (auto& note : notes)
            {
                if (!note.active) continue;
                if (note.lane < 1 || note.lane > 3) continue;
                float cx = LANE_CENTERS[note.lane - 1] + (float)shakeOffsetX;
                float currentY = HIT_Y - ((float)note.time_ms - currentTimeMs) * NOTE_SPEED + (float)shakeOffsetY;
                bool  isHolding = (note.type == NoteType::HOLD && note.holdState == HoldState::HOLDING);
                float bottomEdge = (note.type == NoteType::HOLD)
                    ? currentY - (float)note.duration_ms * NOTE_SPEED  // tail (top of bar)
                    : currentY;
                if (!isHolding && (currentY < -140.0f || bottomEdge > SCREEN_H + 60.0f))
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
                        float remainMs = (float)(note.time_ms + note.duration_ms) - currentTimeMs;
                        tailY = HIT_Y - remainMs * NOTE_SPEED;
                    }
                    else
                    {
                        headY = currentY;
                        tailY = currentY - (float)note.duration_ms * NOTE_SPEED;
                    }
                    float topY = tailY < headY ? tailY : headY;
                    float botY = tailY < headY ? headY : tailY;
                    float bodyH = botY - topY;
                    if (bodyH > 0)
                    {
                        DrawRectangle((int)(cx - 30.0f), (int)topY, 60, (int)bodyH,
                            isHolding ? Color{ 25, 190, 240, 18 } : Color{ 255, 212, 55, 18 });
                        DrawRectangle((int)(cx - 24.0f), (int)topY, 48, (int)bodyH,
                            isHolding ? Color{ 25, 190, 240, 30 } : Color{ 255, 212, 55, 30 });
                        DrawRectangle((int)(cx - 18.0f), (int)topY, 36, (int)bodyH,
                            isHolding ? Color{ 25, 190, 240, 210 } : Color{ 255, 212, 55, 135 });
                        DrawLineEx({ cx - 22.0f, topY }, { cx - 22.0f, botY },
                            3.0f, { 255, 230, 90, 210 });
                        DrawLineEx({ cx + 22.0f, topY }, { cx + 22.0f, botY },
                            3.0f, { 255, 230, 90, 210 });
                    }
                    // Head (at bottom / HIT_Y when holding)
                    DrawCircle((int)cx, (int)headY, 56, { 255, 235, 80, 10 });
                    DrawCircle((int)cx, (int)headY, 46, { 255, 235, 80, 20 });
                    DrawCircle((int)cx, (int)headY, 36, { 255, 235, 80, 45 });
                    DrawRing({ cx, headY }, 22.0f, 30.0f, 0.0f, 360.0f, 72, { 255, 236, 60, 255 });
                    DrawCircle((int)cx, (int)headY, 17, { 65, 70, 70, 230 });
                    // Tail (top end)
                    DrawRing({ cx, tailY }, 17.0f, 23.0f, 0.0f, 360.0f, 72, { 255, 236, 100, 210 });
                    break;
                }
                case NoteType::TRAP:
                {
                    float pulse = 0.5f + 0.5f * sinf(currentTimeMs * 0.008f);
                    // Pulsing outer danger glow
                    DrawCircle((int)cx, (int)currentY, 58, { 255, 25,  8, (unsigned char)(15 + (int)(pulse * 25)) });
                    DrawCircle((int)cx, (int)currentY, 46, { 255, 50, 20, (unsigned char)(28 + (int)(pulse * 38)) });
                    DrawCircle((int)cx, (int)currentY, 36, { 255, 70, 30, (unsigned char)(45 + (int)(pulse * 45)) });
                    // Outer ring — color shifts orange→red with pulse
                    DrawRing({ cx, currentY }, 24.0f, 34.0f, 0.0f, 360.0f, 72,
                        { 255, (unsigned char)(55 + (int)(pulse * 45)), 25, 255 });
                    // Dark core
                    DrawCircle((int)cx, (int)currentY, 22, { 50, 14, 14, 252 });
                    // Inner accent ring
                    DrawRing({ cx, currentY }, 11.0f, 15.0f, 0.0f, 360.0f, 48,
                        { 255, (unsigned char)(145 + (int)(pulse * 65)), 45, 255 });
                    // "!" warning — flickers brighter with pulse
                    drawTxt("!", (int)(cx - 5.0f), (int)(currentY - 20.0f), 30,
                        { 255, (unsigned char)(100 + (int)(pulse * 110)), 50, 255 });
                    // 4 corner spikes radiating outward
                    {
                        float sR = 30.0f, sL = 9.0f;
                        unsigned char sA = (unsigned char)(170 + (int)(pulse * 70));
                        for (int d = 0; d < 4; d++)
                        {
                            float ang = (45.0f + d * 90.0f) * DEG2RAD;
                            DrawLineEx({ cx + cosf(ang) * sR,        currentY + sinf(ang) * sR        },
                                       { cx + cosf(ang) * (sR + sL), currentY + sinf(ang) * (sR + sL) },
                                       3.0f, { 255, 65, 25, sA });
                        }
                    }
                    break;
                }
                default: // NORMAL
                    DrawCircle((int)cx, (int)currentY, 52, { lc.r, lc.g, lc.b, 12 });
                    DrawCircle((int)cx, (int)currentY, 42, { lc.r, lc.g, lc.b, 22 });
                    DrawCircle((int)cx, (int)currentY, 34, { lc.r, lc.g, lc.b, 45 });
                    DrawRing({ cx, currentY }, 17.0f, 24.0f, 0.0f, 360.0f, 72, lc);
                    DrawCircle((int)cx, (int)currentY, 14, { 38, 45, 48, 240 });
                    DrawRing({ cx, currentY }, 13.0f, 15.0f, 0.0f, 360.0f, 72, { 245, 250, 250, 70 });
                    break;
                }
            }

            // Screen flash on trap hit
            if (flashAlpha > 0.0f)
                DrawRectangle(0, 0, SCREEN_W, SCREEN_H, Fade(RED, flashAlpha));

            // Song progress bar (very top of screen)
            {
                float total = musicOk ? GetMusicTimeLength(music) * 1000.0f : 1.0f;
                float progress = (total > 0.0f) ? (currentTimeMs / total) : 0.0f;
                if (progress > 1.0f) progress = 1.0f;
                int barW = (int)(SCREEN_W * progress);
                DrawRectangle(0, 0, SCREEN_W, 6, { 25, 28, 45, 210 });
                if (barW > 0)
                    DrawRectangleGradientH(0, 0, barW, 6, { 70, 110, 255, 200 }, { 140, 210, 255, 240 });
                if (barW > 2)
                    DrawRectangle(barW - 2, 0, 4, 6, { 220, 235, 255, 255 });
            }

            // HUD top-left — score with pop
            {
                std::string stxt = "Счёт: " + std::to_string(score);
                float tp = (scorePopTimer > 0.0f) ? scorePopTimer / 0.18f : 0.0f;
                int   sz = (int)(38.0f * (1.0f + tp * (scorePopPerf ? 0.33f : 0.15f)));
                int   dy = (sz - 38) / 2;
                Color col = {
                    (unsigned char)(245.0f + tp * 10.0f),
                    (unsigned char)(248.0f + tp * (scorePopPerf ? -33.0f :  7.0f)),
                    (unsigned char)(248.0f + tp * (scorePopPerf ? -168.0f : 7.0f)),
                    245
                };
                drawTxt(stxt.c_str(), 22, 18 - dy, sz, col);
            }
            // Combo — small label + large number
            {
                float tp  = (comboPopTimer > 0.0f) ? comboPopTimer / 0.18f : 0.0f;
                int   nsz = (int)(52.0f * (1.0f + tp * (comboPopPerf ? 0.38f : 0.18f)));
                int   dy  = (nsz - 52) / 2;
                Color numCol = {
                    (unsigned char)(255.0f),
                    (unsigned char)(220.0f + tp * (comboPopPerf ?   0.0f : 35.0f)),
                    (unsigned char)( 80.0f + tp * (comboPopPerf ? -80.0f : 175.0f)),
                    (unsigned char)(combo > 0 ? 240 : 110)
                };
                std::string num = "x" + std::to_string(combo);
                drawTxt(num.c_str(), 20, 68 - dy, nsz, numCol);
            }


            // Judgment popup — fade + upward drift above hit line
            if (judgmentTimer > 0.0f)
            {
                const char* label = "ПРОМАХ";  Color col = { 220, 80, 80, 255 };
                if (lastJudgment == Judgment::PERFECT) { label = "ИДЕАЛЬНО"; col = { 255, 215,   0, 255 }; }
                else if (lastJudgment == Judgment::GOOD) { label = "ХОРОШО";   col = { 100, 220, 100, 255 }; }
                else if (lastJudgment == Judgment::TRAP) { label = "ЛОВУШКА";  col = { 255,  60,  60, 255 }; }
                else if (lastJudgment == Judgment::HOLD_OK) { label = "УДЕРЖАНО"; col = { 60, 220, 120, 255 }; }
                else if (lastJudgment == Judgment::HOLD_MISS) { label = "СРЫВ";     col = { 200, 100, 255, 255 }; }

                float t = judgmentTimer / 1.5f;
                unsigned char alpha;
                if (t > 0.85f) alpha = (unsigned char)(((1.0f - t) / 0.15f) * 255.0f);
                else if (t < 0.25f) alpha = (unsigned char)((t / 0.25f) * 255.0f);
                else                alpha = 255;
                col.a = alpha;

                float yDrift = -(1.5f - judgmentTimer) * 14.0f;
                float yPos = HIT_Y + 28.0f + yDrift;
                float fSize = 48.0f;
                Vector2 jSz = MeasureTextEx(font, label, fSize, 1.0f);
                DrawTextEx(font, label, { SCREEN_W / 2.0f - jSz.x / 2, yPos }, fSize, 1.0f, col);
            }

            // ============================================================
            // PAUSED overlay (drawn on top of frozen gameplay)
            // ============================================================
            if (state == GameState::PAUSED)
            {
                // Input — skip all key checks on the frame we just entered pause
                if (justPaused) { justPaused = false; }
                else
                {
                    if (IsKeyPressed(KEY_ESCAPE))
                    {
                        if (musicOk) ResumeMusicStream(music);
                        state = GameState::GAMEPLAY;
                    }
                    if (IsKeyPressed(KEY_UP))   pauseSelection = (pauseSelection + 2) % 3;
                    if (IsKeyPressed(KEY_DOWN)) pauseSelection = (pauseSelection + 1) % 3;
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
                        else { running = false; }
                    }
                } // end else (not justPaused)

                // Dim overlay
                DrawRectangle(0, 0, SCREEN_W, SCREEN_H, { 0, 0, 0, 150 });

                // Panel
                int pw = 420, ph = 260;
                int px = SCREEN_W / 2 - pw / 2, py = SCREEN_H / 2 - ph / 2;
                DrawRectangle(px, py, pw, ph, { 20, 22, 35, 240 });
                DrawRectangleLines(px, py, pw, ph, { 80, 80, 140, 200 });

                // Title
                Vector2 pSz = MeasureTextEx(font, "Пауза", 52.0f, 1.0f);
                DrawTextEx(font, "Пауза",
                    { SCREEN_W / 2.0f - pSz.x / 2, (float)py + 20 }, 52.0f, 1.0f, WHITE);

                // Menu items
                const char* pItems[3] = { "Продолжить", "Главное меню", "Выйти из игры" };
                for (int i = 0; i < 3; i++)
                {
                    Color c = (i == pauseSelection)
                        ? Color{ 255, 230, 80, 255 } : Color{ 180, 185, 195, 255 };
                    Vector2 sz = MeasureTextEx(font, pItems[i], 28.0f, 1.0f);
                    float by = (float)(py + 88 + i * 50);
                    if (i == pauseSelection)
                        DrawRectangle(px + 20, (int)(by - 4), pw - 40, 40, { 255, 255, 255, 20 });
                    DrawTextEx(font, pItems[i],
                        { SCREEN_W / 2.0f - sz.x / 2, by }, 28.0f, 1.0f, c);
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
                    DrawTextEx(font, t, { SCREEN_W / 2.0f - s.x / 2, y }, (float)sz, 1.0f, c);
                };

            Vector2 rSz = MeasureTextEx(font, "РЕЗУЛЬТАТ", 72.0f, 2.0f);
            DrawTextEx(font, "РЕЗУЛЬТАТ",
                { SCREEN_W / 2.0f - rSz.x / 2, 40 }, 72.0f, 2.0f, WHITE);
            DrawLineEx({ SCREEN_W / 2.0f - 260, 132 }, { SCREEN_W / 2.0f + 260, 132 },
                2.0f, { 80, 80, 130, 200 });

            DrawRectangle(SCREEN_W / 2 - 244, 144, 488, 394, { 0, 0, 0, 155 });
            DrawRectangleLines(SCREEN_W / 2 - 244, 144, 488, 394, { 55, 55, 105, 180 });

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

            int sy = 158, sx = SCREEN_W / 2 - 224;
            int vx = sx + 240; // fixed column for values
            drawTxt("Счёт:",           sx, sy, 30, WHITE);   drawTxt(std::to_string(score).c_str(), vx, sy, 30, WHITE);   sy+=46;
            drawTxt("Точность:",       sx, sy, 26, accCol);  drawTxt(accBuf,                        vx, sy, 26, accCol);  sy+=40;
            drawTxt("Макс. комбо:",    sx, sy, 24, {255,200, 50,255}); drawTxt(std::to_string(maxCombo).c_str(),     vx, sy, 24, {255,200, 50,255}); sy+=36;
            drawTxt("Идеально:",       sx, sy, 24, {255,215,  0,255}); drawTxt(std::to_string(perfectCount).c_str(), vx, sy, 24, {255,215,  0,255}); sy+=34;
            drawTxt("Хорошо:",         sx, sy, 24, {100,220,100,255}); drawTxt(std::to_string(goodCount).c_str(),    vx, sy, 24, {100,220,100,255}); sy+=34;
            drawTxt("Промах:",         sx, sy, 24, {220, 80, 80,255}); drawTxt(std::to_string(missCount).c_str(),    vx, sy, 24, {220, 80, 80,255}); sy+=34;
            drawTxt("Ловушки:",        sx, sy, 24, {255,100, 80,255}); drawTxt(std::to_string(trapErrors).c_str(),   vx, sy, 24, {255,100, 80,255}); sy+=34;
            drawTxt("Удержано:",       sx, sy, 24, { 60,220,120,255}); drawTxt(std::to_string(holdOkCount).c_str(),  vx, sy, 24, { 60,220,120,255}); sy+=34;
            drawTxt("Срыв удержания:", sx, sy, 24, {200,100,255,255}); drawTxt(std::to_string(holdMissCount).c_str(),vx, sy, 24, {200,100,255,255});

            DrawLineEx({ SCREEN_W / 2.0f - 200, 554 }, { SCREEN_W / 2.0f + 200, 554 },
                1.0f, { 60, 60, 100, 150 });
            cTxt("R — начать заново", 566, 26, { 200, 200, 255, 255 });
            cTxt("ESC — в меню",      596, 26, { 140, 140, 180, 255 });
        }

        EndTextureMode();

        // Scale virtual 1280×720 canvas to actual screen
        float sw = (float)GetScreenWidth();
        float sh = (float)GetScreenHeight();
        float sc = (sw / SCREEN_W < sh / SCREEN_H) ? sw / SCREEN_W : sh / SCREEN_H;
        float ox = (sw - SCREEN_W * sc) * 0.5f;
        float oy = (sh - SCREEN_H * sc) * 0.5f;

        BeginDrawing();
        ClearBackground(BLACK);
        DrawTexturePro(canvas.texture,
            { 0.0f, 0.0f, (float)SCREEN_W, -(float)SCREEN_H },
            { ox, oy, (float)SCREEN_W * sc, (float)SCREEN_H * sc },
            { 0.0f, 0.0f }, 0.0f, WHITE);
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