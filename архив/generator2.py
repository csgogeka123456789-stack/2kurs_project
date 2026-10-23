import librosa
import numpy as np
import json
import random

def generate_beatmap(audio_file, output_json, lanes_count=3):
    print(f"Анализ трека: {audio_file}...")
    
    # Загружаем аудиофайл
    y, sr = librosa.load(audio_file, sr=None)
    
    # Ищем биты с помощью улучшенного анализатора (onset_strength)
    onset_env = librosa.onset.onset_strength(y=y, sr=sr)
    tempo, beat_frames = librosa.beat.beat_track(onset_envelope=onset_env, sr=sr)
    beat_times_sec = librosa.frames_to_time(beat_frames, sr=sr)
    
    # Рассчитываем и нормализуем энергию (громкость) от 0 до 1
    rms = librosa.feature.rms(y=y)[0]
    if max(rms) > 0:
        rms = rms / max(rms)
    
    notes = []
    
    for i in range(len(beat_frames)):
        time_ms = int(beat_times_sec[i] * 1000)
        
        # Пропускаем первые 2 секунды (даем игроку приготовиться)
        if time_ms < 2000:
            continue
            
        current_frame = min(beat_frames[i], len(rms) - 1)
        next_frame = beat_frames[i+1] if i + 1 < len(beat_frames) else len(rms) - 1
        
        start_energy = rms[current_frame]
        
        # --- СТРОГИЙ АНАЛИЗ ДЛИТЕЛЬНОСТИ ---
        # Порог 60%: звук должен быть действительно громким, чтобы считаться "тянущимся"
        energy_threshold = start_energy * 0.60 
        
        end_frame = current_frame
        for f in range(current_frame + 1, next_frame):
            if rms[f] < energy_threshold:
                break
            end_frame = f
            
        duration_ms = int(librosa.frames_to_time([end_frame - current_frame], sr=sr)[0] * 1000)
        lane = random.randint(1, lanes_count)
        
        # --- ЛОГИКА ВЫБОРА ТИПА НОТЫ ---
        HOLD_CHANCE = 0.3 # Даже если звук длинный, только 30% станут холдами
        MIN_HOLD_MS = 350 # Звук должен тянуться минимум 350 мс
        
        if duration_ms >= MIN_HOLD_MS and random.random() < HOLD_CHANCE:
            # Ограничиваем длину: отступаем 100 мс от следующей ноты
            max_duration = int((librosa.frames_to_time([next_frame - current_frame], sr=sr)[0]) * 1000) - 100
            final_duration = max(250, min(duration_ms, max_duration))
            
            notes.append({
                "time_ms": time_ms,
                "lane": lane,
                "type": "hold",
                "duration_ms": final_duration
            })
        else:
            # Если это не холд: с вероятностью 90% это обычная нота, 10% - ловушка
            note_type = "trap" if random.random() > 0.90 else "normal"
            notes.append({
                "time_ms": time_ms,
                "lane": lane,
                "type": note_type
            })
    
    # Собираем итоговый JSON
    bpm_val = int(tempo[0]) if isinstance(tempo, np.ndarray) else int(tempo)
    level_data = {
        "song": audio_file,
        "bpm": bpm_val,
        "lanes": [
            { "lane": 1, "key": "A" },
            { "lane": 2, "key": "S" },
            { "lane": 3, "key": "D" }
        ],
        "notes": notes
    }
    
    # Сохраняем в файл
    with open(output_json, 'w', encoding='utf-8') as f:
        json.dump(level_data, f, indent=2, ensure_ascii=False)
        
    holds = [n for n in notes if n["type"] == "hold"]
    print(f"Готово! Найдено нот: {len(notes)} (из них Hold: {len(holds)}). BPM: {level_data['bpm']}")
    print(f"Карта сохранена в: {output_json}")

if __name__ == "__main__":
    generate_beatmap("track2.ogg", "example2.json")