#include "raylib.h"
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------
constexpr float NOTE_SPEED  = 0.5f;    // pixels per millisecond
constexpr float HIT_Y       = 600.0f;  // y-coordinate of the hit line
constexpr float NOTE_RADIUS = 18.0f;
constexpr float LANE_WIDTH  = 80.0f;

// Lane centers (3 lanes, evenly spaced around screen center)
const float LANE_CENTERS[3] = { 480.0f, 640.0f, 800.0f };

// ---------------------------------------------------------------------------
// Note
// ---------------------------------------------------------------------------
enum class NoteType  { NORMAL, TRAP, HOLD };
enum class HoldState { IDLE, HOLDING, COMPLETED, FAILED };

struct Note
{
    int      time_ms;
    int      lane;          // 1-based
    NoteType type;
    int      duration_ms;   // only relevant for HOLD
    bool      active    = true;
    HoldState holdState  = HoldState::IDLE;  // only used for HOLD type
};

// ---------------------------------------------------------------------------
// Judgment
// ---------------------------------------------------------------------------
enum class GameState { MENU, GAMEPLAY, RESULT };
enum class Judgment  { NONE, PERFECT, GOOD, MISS, TRAP, HOLD_OK, HOLD_MISS };

// Hit windows calibrated to NOTE_SPEED=0.5 px/ms and NOTE_RADIUS=18px:
//   PERFECT 45ms → note 22.5px from line → bottom edge 4.5px above (visually touching)
//   GOOD    90ms → note 45px from line  → bottom edge 27px above  (clearly approaching)
constexpr int HIT_WINDOW_PERFECT = 45;   // ±45 ms
constexpr int HIT_WINDOW_GOOD    = 90;   // ±90 ms
constexpr int HIT_WINDOW_MISS    = 200;  // ±200 ms — auto-miss / ghost-tap boundary

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
        size_t objEnd   = src.find('}', pos);
        if (objStart == std::string::npos || objEnd == std::string::npos) break;
        if (objStart > objEnd) break;

        std::string obj = src.substr(objStart + 1, objEnd - objStart - 1);

        Note note{};
        note.time_ms     = jsonInt(obj, "time_ms");
        note.lane        = jsonInt(obj, "lane");
        note.duration_ms = jsonInt(obj, "duration_ms", 0);

        std::string t    = jsonStr(obj, "type");
        if      (t == "trap") note.type = NoteType::TRAP;
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

    RenderTexture2D canvas = LoadRenderTexture(SCREEN_W, SCREEN_H);
    SetTextureFilter(canvas.texture, TEXTURE_FILTER_BILINEAR);

    // --- Load assets ---
    Music music = LoadMusicStream("assets/track.ogg");

    // Font with ASCII + full Cyrillic block
    const int CP_COUNT = 96 + 256 + 1;
    int codepoints[CP_COUNT];
    for (int i = 0; i < 96;  i++) codepoints[i]      = 32 + i;
    for (int i = 0; i < 256; i++) codepoints[96 + i] = 0x0400 + i;
    codepoints[96 + 256] = 0x2014;  // em dash —
    Font font = LoadFontEx("assets/font.ttf", 64, codepoints, CP_COUNT);
    SetTextureFilter(font.texture, TEXTURE_FILTER_BILINEAR);

    bool musicOk = (music.frameCount > 0);
    bool fontOk  = (font.texture.id  > 0);

    // --- Parse chart ---
    int               bpm = 0;
    std::vector<Note> notes;
    bool chartOk = parseChart("assets/example.json", bpm, notes);

    // --- Game state ---
    GameState state        = GameState::MENU;
    int      score         = 0;
    int      perfectCount  = 0;
    int      goodCount     = 0;
    int      missCount     = 0;
    int      trapErrors    = 0;
    int      holdOkCount   = 0;
    int      holdMissCount = 0;
    int      combo         = 0;
    int      maxCombo      = 0;
    Judgment lastJudgment  = Judgment::NONE;
    float    judgmentTimer = 0.0f;

    // Helper: draw text with loaded font, fallback to default raylib font
    auto drawTxt = [&](const char* text, int x, int y, int size, Color col)
    {
        if (fontOk)
            DrawTextEx(font, text, { (float)x, (float)y }, (float)size, 1.0f, col);
        else
            DrawText(text, x, y, size, col);
    };

    // Reset all game data and (re)start the track
    auto resetGame = [&]()
    {
        score = perfectCount = goodCount = missCount = 0;
        trapErrors = holdOkCount = holdMissCount = combo = maxCombo = 0;
        lastJudgment  = Judgment::NONE;
        judgmentTimer = 0.0f;
        for (auto& n : notes) { n.active = true; n.holdState = HoldState::IDLE; }
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
            if (IsKeyPressed(KEY_ENTER)) resetGame();

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

            cTxt("Нажмите ENTER для начала",         282, 30, { 200, 200, 255, 255 });
            cTxt("A / S / D — нажимать ноты",           344, 22, { 140, 140, 190, 255 });
            cTxt("Красные ноты — ЛОВУШКИ, не нажимать!", 372, 22, { 220,  80,  80, 255 });
            cTxt("Золотые ноты — удерживать до конца",   400, 22, { 255, 200,  40, 255 });

            if (!musicOk) cTxt("ОШИБКА: track.ogg не найден",    510, 22, RED);
            if (!chartOk) cTxt("ОШИБКА: example.json не найден", 538, 22, RED);
        }
        // ================================================================
        // GAMEPLAY
        // ================================================================
        else if (state == GameState::GAMEPLAY)
        {
            // --- Update ---
            if (musicOk) UpdateMusicStream(music);
            float currentTimeMs = musicOk ? GetMusicTimePlayed(music) * 1000.0f : 0.0f;

            // Auto-miss: normal/hold-idle notes that passed start window
            // Trap notes that passed → silent deactivation
            for (auto& note : notes)
            {
                if (!note.active) continue;
                if (note.type == NoteType::HOLD && note.holdState == HoldState::HOLDING) continue;
                if ((currentTimeMs - (float)note.time_ms) > (float)HIT_WINDOW_MISS)
                {
                    note.active = false;
                    if (note.type != NoteType::TRAP)
                    {
                        missCount++;
                        combo         = 0;
                        lastJudgment  = Judgment::MISS;
                        judgmentTimer = 1.5f;
                    }
                }
            }

            // HOLDING notes: check key still down each frame
            for (auto& note : notes)
            {
                if (!note.active)                         continue;
                if (note.type      != NoteType::HOLD)     continue;
                if (note.holdState != HoldState::HOLDING) continue;

                bool keyDown = (note.lane == 1 && IsKeyDown(KEY_A)) ||
                               (note.lane == 2 && IsKeyDown(KEY_S)) ||
                               (note.lane == 3 && IsKeyDown(KEY_D));
                float endMs  = (float)(note.time_ms + note.duration_ms);

                if (currentTimeMs >= endMs)
                {
                    note.active    = false;
                    note.holdState = HoldState::COMPLETED;
                    score         += 150;
                    holdOkCount++;
                    combo++;
                    if (combo > maxCombo) maxCombo = combo;
                    lastJudgment  = Judgment::HOLD_OK;
                    judgmentTimer = 1.5f;
                }
                else if (!keyDown)
                {
                    note.active    = false;
                    note.holdState = HoldState::FAILED;
                    holdMissCount++;
                    combo         = 0;
                    lastJudgment  = Judgment::HOLD_MISS;
                    judgmentTimer = 1.5f;
                }
            }

            // Input: A=lane1, S=lane2, D=lane3
            auto processLane = [&](int lane)
            {
                // Find the earliest active, non-holding note within the miss window
                int   bestIdx  = -1;
                float bestTime = 1e30f;
                for (int i = 0; i < (int)notes.size(); i++)
                {
                    if (!notes[i].active)      continue;
                    if (notes[i].lane != lane) continue;
                    if (notes[i].type == NoteType::HOLD &&
                        notes[i].holdState == HoldState::HOLDING) continue;
                    float d       = currentTimeMs - (float)notes[i].time_ms;
                    float absDiff = d < 0.0f ? -d : d;
                    if (absDiff > (float)HIT_WINDOW_MISS) continue;
                    if ((float)notes[i].time_ms < bestTime)
                    {
                        bestTime = (float)notes[i].time_ms;
                        bestIdx  = i;
                    }
                }

                // Ghost tap: no note within the miss window
                if (bestIdx == -1)
                {
                    score        = (score >= 50) ? score - 50 : 0;
                    missCount++;
                    combo        = 0;
                    lastJudgment  = Judgment::MISS;
                    judgmentTimer = 1.5f;
                    return;
                }

                float d       = currentTimeMs - (float)notes[bestIdx].time_ms;
                float absDiff = d < 0.0f ? -d : d;

                if (notes[bestIdx].type == NoteType::TRAP)
                {
                    notes[bestIdx].active = false;
                    score        = (score >= 100) ? score - 100 : 0;
                    trapErrors++;
                    combo         = 0;
                    lastJudgment  = Judgment::TRAP;
                    judgmentTimer = 1.5f;
                }
                else if (notes[bestIdx].type == NoteType::HOLD &&
                         notes[bestIdx].holdState == HoldState::IDLE)
                {
                    notes[bestIdx].holdState = HoldState::HOLDING;
                    if (absDiff <= (float)HIT_WINDOW_PERFECT) { score += 100; perfectCount++; lastJudgment = Judgment::PERFECT; }
                    else                                       { score +=  50; goodCount++;    lastJudgment = Judgment::GOOD;    }
                    combo++;
                    if (combo > maxCombo) maxCombo = combo;
                    judgmentTimer = 1.0f;
                }
                else if (absDiff <= (float)HIT_WINDOW_PERFECT)
                {
                    notes[bestIdx].active = false;
                    score += 100; perfectCount++;
                    combo++; if (combo > maxCombo) maxCombo = combo;
                    lastJudgment  = Judgment::PERFECT;
                    judgmentTimer = 1.5f;
                }
                else if (absDiff <= (float)HIT_WINDOW_GOOD)
                {
                    notes[bestIdx].active = false;
                    score += 50; goodCount++;
                    combo++; if (combo > maxCombo) maxCombo = combo;
                    lastJudgment  = Judgment::GOOD;
                    judgmentTimer = 1.5f;
                }
                // else: note exists but is outside GOOD window — press ignored, note keeps falling
            };

            if (IsKeyPressed(KEY_A)) processLane(1);
            if (IsKeyPressed(KEY_S)) processLane(2);
            if (IsKeyPressed(KEY_D)) processLane(3);

            if (judgmentTimer > 0.0f) judgmentTimer -= dt;

            // Song end: all notes gone or music stopped
            bool allDone = true;
            for (const auto& n : notes) if (n.active) { allDone = false; break; }
            if (allDone || (musicOk && !IsMusicStreamPlaying(music) && currentTimeMs > 500.0f))
                state = GameState::RESULT;

            // --- Draw gameplay ---
            // Lanes
            for (int i = 0; i < 3; i++)
            {
                float lx = LANE_CENTERS[i] - LANE_WIDTH * 0.5f;
                DrawRectangle((int)lx, 0, (int)LANE_WIDTH, SCREEN_H, { 22, 22, 40, 255 });
                DrawRectangleLines((int)lx, 0, (int)LANE_WIDTH, SCREEN_H, { 55, 55, 90, 255 });
            }
            // Hit line + soft glow
            DrawLineEx({ 0.0f, HIT_Y + 2 }, { (float)SCREEN_W, HIT_Y + 2 }, 1.0f, { 130, 130, 210, 70 });
            DrawLineEx({ 0.0f, HIT_Y      }, { (float)SCREEN_W, HIT_Y      }, 3.0f, { 210, 210, 255, 255 });

            // Notes
            for (const auto& note : notes)
            {
                if (!note.active) continue;
                float cx       = LANE_CENTERS[note.lane - 1];
                float currentY = HIT_Y - ((note.time_ms - currentTimeMs) * NOTE_SPEED);
                bool  isHolding = (note.type == NoteType::HOLD && note.holdState == HoldState::HOLDING);
                if (!isHolding && (currentY < -100.0f || currentY > 820.0f)) continue;

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
                    else { headY = currentY; tailY = currentY - (float)note.duration_ms * NOTE_SPEED; }
                    float bodyH = headY - tailY;
                    if (bodyH > 0)
                        DrawRectangle(
                            (int)(cx - NOTE_RADIUS * 0.55f), (int)tailY,
                            (int)(NOTE_RADIUS * 1.1f),        (int)bodyH,
                            isHolding ? Color{ 200, 155, 20, 180 } : Color{ 200, 155, 20, 140 });
                    DrawCircle((int)cx, (int)tailY, (int)(NOTE_RADIUS * 0.55f), { 230, 190, 60, 200 });
                    DrawCircle((int)cx, (int)headY, NOTE_RADIUS,
                               isHolding ? Color{ 255, 220, 40, 255 } : Color{ 220, 180, 40, 255 });
                    DrawCircleLines((int)cx, (int)headY, NOTE_RADIUS + 3, { 255, 240, 160, 180 });
                    break;
                }
                case NoteType::TRAP:
                    DrawCircle((int)cx, (int)currentY, NOTE_RADIUS, { 255,  50,  60, 255 });
                    DrawCircleLines((int)cx, (int)currentY, NOTE_RADIUS + 3, { 255, 130, 130, 200 });
                    break;
                default: // NORMAL
                    DrawCircle((int)cx, (int)currentY, NOTE_RADIUS, {  70, 130, 255, 255 });
                    DrawCircleLines((int)cx, (int)currentY, NOTE_RADIUS + 3, { 160, 200, 255, 200 });
                    break;
                }
            }

            // HUD left
            drawTxt(("BPM: " + std::to_string(bpm)).c_str(), 20, 18, 24, { 185, 185, 225, 230 });
            drawTxt(("Время: " + std::to_string((int)currentTimeMs / 1000) + " с").c_str(),
                    20, SCREEN_H - 38, 24, { 185, 185, 225, 230 });

            // HUD right: semi-transparent panel
            DrawRectangle(SCREEN_W - 320, 6, 314, 272, { 0, 0, 0, 155 });
            DrawRectangleLines(SCREEN_W - 320, 6, 314, 272, { 55, 55, 105, 180 });
            int ry = 14;
            drawTxt(("Счёт: " + std::to_string(score)).c_str(),     SCREEN_W-308, ry, 32, WHITE);             ry+=46;
            drawTxt(("Комбо: x" + std::to_string(combo)).c_str(),   SCREEN_W-308, ry, 22, {255,200, 50,255}); ry+=28;
            drawTxt(("Идеально: " + std::to_string(perfectCount)).c_str(), SCREEN_W-308, ry, 22, {255,215,  0,255}); ry+=28;
            drawTxt(("Хорошо:   " + std::to_string(goodCount)).c_str(),    SCREEN_W-308, ry, 22, {100,220,100,255}); ry+=28;
            drawTxt(("Промах:   " + std::to_string(missCount)).c_str(),    SCREEN_W-308, ry, 22, {220, 80, 80,255}); ry+=28;
            drawTxt(("Ловушки:  " + std::to_string(trapErrors)).c_str(),   SCREEN_W-308, ry, 22, {255,100, 80,255}); ry+=28;
            drawTxt(("Удержано: " + std::to_string(holdOkCount)).c_str(),  SCREEN_W-308, ry, 22, { 60,220,120,255}); ry+=28;
            drawTxt(("Срыв:     " + std::to_string(holdMissCount)).c_str(), SCREEN_W-308, ry, 22, {200,100,255,255});

            // Judgment popup — below hit line, fade in/out + upward drift
            if (judgmentTimer > 0.0f)
            {
                const char* label = "ПРОМАХ";  Color col = { 220, 80, 80, 255 };
                if      (lastJudgment == Judgment::PERFECT)  { label = "ИДЕАЛЬНО"; col = { 255, 215,   0, 255 }; }
                else if (lastJudgment == Judgment::GOOD)     { label = "ХОРОШО";   col = { 100, 220, 100, 255 }; }
                else if (lastJudgment == Judgment::TRAP)     { label = "ЛОВУШКА";  col = { 255,  60,  60, 255 }; }
                else if (lastJudgment == Judgment::HOLD_OK)  { label = "УДЕРЖАНО"; col = {  60, 220, 120, 255 }; }
                else if (lastJudgment == Judgment::HOLD_MISS){ label = "СРЫВ";     col = { 200, 100, 255, 255 }; }

                float t = judgmentTimer / 1.5f;
                unsigned char alpha;
                if      (t > 0.85f) alpha = (unsigned char)(((1.0f - t) / 0.15f) * 255.0f);
                else if (t < 0.25f) alpha = (unsigned char)((t / 0.25f) * 255.0f);
                else                alpha = 255;
                col.a = alpha;

                float yDrift = -(1.5f - judgmentTimer) * 14.0f;
                float yPos   = HIT_Y + 28.0f + yDrift;
                float fSize  = 48.0f;
                Vector2 jSz  = MeasureTextEx(font, label, fSize, 1.0f);
                DrawTextEx(font, label, { SCREEN_W / 2.0f - jSz.x / 2, yPos }, fSize, 1.0f, col);
            }
        }
        // ================================================================
        // RESULT
        // ================================================================
        else if (state == GameState::RESULT)
        {
            if (IsKeyPressed(KEY_R))      resetGame();
            if (IsKeyPressed(KEY_ESCAPE)) running = false;

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

            DrawRectangle(SCREEN_W / 2 - 244, 144, 488, 358, { 0, 0, 0, 155 });
            DrawRectangleLines(SCREEN_W / 2 - 244, 144, 488, 358, { 55, 55, 105, 180 });

            int sy = 158, sx = SCREEN_W / 2 - 224;
            drawTxt(("Счёт:                " + std::to_string(score)).c_str(),        sx, sy, 30, WHITE);             sy+=50;
            drawTxt(("Макс. комбо:         " + std::to_string(maxCombo)).c_str(),     sx, sy, 24, {255,200, 50,255}); sy+=36;
            drawTxt(("Идеально:           " + std::to_string(perfectCount)).c_str(), sx, sy, 24, {255,215,  0,255}); sy+=34;
            drawTxt(("Хорошо:             " + std::to_string(goodCount)).c_str(),    sx, sy, 24, {100,220,100,255}); sy+=34;
            drawTxt(("Промах:             " + std::to_string(missCount)).c_str(),    sx, sy, 24, {220, 80, 80,255}); sy+=34;
            drawTxt(("Ловушки:            " + std::to_string(trapErrors)).c_str(),   sx, sy, 24, {255,100, 80,255}); sy+=34;
            drawTxt(("Удержано:           " + std::to_string(holdOkCount)).c_str(),  sx, sy, 24, { 60,220,120,255}); sy+=34;
            drawTxt(("Срыв удержания:      " + std::to_string(holdMissCount)).c_str(), sx, sy, 24, {200,100,255,255});

            DrawLineEx({ SCREEN_W / 2.0f - 200, 518 }, { SCREEN_W / 2.0f + 200, 518 },
                       1.0f, { 60, 60, 100, 150 });
            cTxt("R — начать заново",   530, 26, { 200, 200, 255, 255 });
            cTxt("ESC — выйти",          562, 26, { 140, 140, 180, 255 });
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

// СЛЕДУЮЩИЙ ЭТАП: экран результатов по окончании трека (итоговый счёт, S/A/B/C ранг),
// перезапуск уровня по клавише R.
