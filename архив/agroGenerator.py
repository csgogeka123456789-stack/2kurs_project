import librosa
import numpy as np
import json
import random

def generate_beatmap(audio_file, output_json, lanes_count=3):
    # --- НАСТРОЙКИ ПЛОТНОСТИ ХОЛДОВ ---
    FORCE_HOLD_CHANCE = 0.25  # 25% шанс превратить ЛЮБУЮ ноту в холд
    MIN_HOLD_MS = 200         # Минимальная длина зажатия
    ENERGY_THRESHOLD = 0.15   # Порог тишины (чем меньше, тем длиннее холды)
    # ----------------------------------

    print(f"Анализ трека: {audio_file}...")
    y, sr = librosa.load(audio_file, sr=None)
    
    # Ищем биты
    onset_env = librosa.onset.onset_strength(y=y, sr=sr)
    tempo, beat_frames = librosa.beat.beat_track(onset_envelope=onset_env, sr=sr)
    beat_times_sec = librosa.frames_to_time(beat_frames, sr=sr)
    
    # Громкость (RMS)
    rms = librosa.feature.rms(y=y)[0]
    if max(rms) > 0: rms = rms / max(rms) # Нормализация

    notes = []
    
    for i in range(len(beat_frames)):
        time_ms = int(beat_times_sec[i] * 1000)
        if time_ms < 2000: continue # Пропуск начала
            
        curr_f = min(beat_frames[i], len(rms) - 1)
        next_f = beat_frames[i+1] if i + 1 < len(beat_frames) else len(rms) - 1
        
        # Расстояние до следующего удара (в мс)
        gap_to_next = int(librosa.frames_to_time([next_f - curr_f], sr=sr)[0] * 1000)
        
        # Анализ затухания
        limit = rms[curr_f] * ENERGY_THRESHOLD
        end_f = curr_f
        for f in range(curr_f + 1, next_f):
            if rms[f] < limit: break
            end_f = f
        
        detected_duration = int(librosa.frames_to_time([end_f - curr_f], sr=sr)[0] * 1000)
        lane = random.randint(1, lanes_count)

        # ЛОГИКА ОПРЕДЕЛЕНИЯ ТИПА
        # Если звук реально длинный ИЛИ сработал шанс FORCE_HOLD_CHANCE
        if detected_duration >= MIN_HOLD_MS or random.random() < FORCE_HOLD_CHANCE:
            # Делаем холд длиной от 50% до 80% расстояния до следующей ноты
            final_duration = max(detected_duration, int(gap_to_next * random.uniform(0.5, 0.8)))
            
            notes.append({
                "time_ms": time_ms,
                "lane": lane,
                "type": "hold",
                "duration_ms": final_duration
            })
        else:
            # Обычная нота или ловушка
            n_type = "trap" if random.random() < 0.1 else "normal"
            notes.append({ "time_ms": time_ms, "lane": lane, "type": n_type })

    # Сохранение
    bpm_val = int(tempo[0]) if isinstance(tempo, np.ndarray) else int(tempo)
    level_data = {
        "song": audio_file,
        "bpm": bpm_val,
        "lanes": [
            { "lane": 1, "key": "A" }, { "lane": 2, "key": "S" }, { "lane": 3, "key": "D" }
        ],
        "notes": notes
    }
    
    with open(output_json, 'w', encoding='utf-8') as f:
        json.dump(level_data, f, indent=2, ensure_ascii=False)
    
    holds = [n for n in notes if n["type"] == "hold"]
    print(f"Результат: Всего {len(notes)} нот, из них {len(holds)} холдов.")

if __name__ == "__main__":
    generate_beatmap("track.ogg", "example.json")