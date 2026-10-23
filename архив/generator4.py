import librosa
import numpy as np
import json
import random

def generate_balanced_bj_map(audio_file, output_json):
    print(f"🕺 [Синхрон Баланс] Глубокий анализ частот трека: {audio_file}...")
    
    # Загружаем аудио
    y, sr = librosa.load(audio_file, sr=None)
    
    tempo, _ = librosa.beat.beat_track(y=y, sr=sr)
    bpm_val = int(tempo[0]) if isinstance(tempo, np.ndarray) else int(tempo)
    
    print("🎹 Разделяем инструменты: Ударные VS Мелодия...")
    y_harmonic, y_percussive = librosa.effects.hpss(y)
    
    # Считаем RMS (громкость) для каждого слоя отдельно
    rms_all = librosa.feature.rms(y=y)[0]
    rms_all = rms_all / max(rms_all) if max(rms_all) > 0 else rms_all
    
    rms_harm = librosa.feature.rms(y=y_harmonic)[0]
    rms_perc = librosa.feature.rms(y=y_percussive)[0]
    
    # Центроид для определения высоты
    centroid = librosa.feature.spectral_centroid(y=y, sr=sr)[0]
    
    # Ищем онсеты (удары)
    onset_frames = librosa.onset.onset_detect(y=y, sr=sr, backtrack=True, wait=6, delta=0.06)
    onset_times_sec = librosa.frames_to_time(onset_frames, sr=sr)
    
    notes = []
    last_trap_time_ms = 0
    
    # Переменные для защиты от спама одной кнопки
    last_lanes = [] 
    
    print("👾 Распределяем ноты по умной геймдизайнерской сетке...")
    for i in range(len(onset_frames)):
        time_ms = int(onset_times_sec[i] * 1000)
        
        if time_ms < 2000: # Интро тишина
            continue
            
        current_frame = min(onset_frames[i], len(rms_all) - 1)
        
        if rms_all[current_frame] < 0.10: # Отсечка тишины
            continue
            
        # Определяем, что доминирует в этом кадре: ударные или мелодия/вокал
        is_percussive = rms_perc[current_frame] > rms_harm[current_frame] * 0.9
        current_centroid = centroid[current_frame]
        
        # --- УЛУЧШЕННОЕ РАСПРЕДЕЛЕНИЕ ПО ДОРОЖКАМ ---
        if is_percussive:
            # Если это ударные: делим строго на бочку (низ) и снейр/хэт (верх)
            if current_centroid < 2200: 
                lane = 1  # Клавиша A (Теперь сюда будет стабильно падать качающая бочка!)
            else:
                lane = 3  # Клавиша D (Снейры и четкие хэты)
        else:
            # Если доминирует вокал или синтезатор — это центральная дорожка
            lane = 2  # Клавиша S
            
        # --- АНТИ-СПАМ СИСТЕМА (БАЛАНСИРОВЩИК ПАЛЬЦЕВ) ---
        last_lanes.append(lane)
        if len(last_lanes) > 3:
            last_lanes.pop(0)
            
        # Если скрипт зажал одну кнопку 3 раза подряд — раскидываем на соседние
        if len(last_lanes) == 3 and last_lanes[0] == last_lanes[1] == last_lanes[2]:
            if lane == 3:
                lane = random.choice([1, 2])
            elif lane == 1:
                lane = random.choice([2, 3])
            else:
                lane = random.choice([1, 3])
            last_lanes[-1] = lane # Обновляем историю
            
        # Считаем расстояние до следующего звука
        if i + 1 < len(onset_times_sec):
            next_time_ms = int(onset_times_sec[i+1] * 1000)
            gap_ms = next_time_ms - time_ms
        else:
            gap_ms = 1000
            
        # --- ОПРЕДЕЛЕНИЕ ТИПА НОТЫ ---
        # Ловушки: под самые жесткие пики вокала или взрывные снейры (кулдаун 2 секунды)
        if rms_all[current_frame] > 0.75 and lane != 1 and (time_ms - last_trap_time_ms > 2000):
            note_type = "trap"
            last_trap_time_ms = time_ms
            notes.append({"time_ms": time_ms, "lane": lane, "type": note_type})
            
        # Холды: теперь разрешены НА ВСЕХ дорожках, если после ноты идет пауза > 320мс
        elif gap_ms > 320:
            duration = int(gap_ms * 0.65) # Держим ноту 65% времени от паузы
            notes.append({
                "time_ms": time_ms,
                "lane": lane,
                "type": "hold",
                "duration_ms": max(150, duration)
            })
            
        # Обычные ноты
        else:
            notes.append({"time_ms": time_ms, "lane": lane, "type": "normal"})

    # Собираем JSON
    level_data = {
        "song": audio_file,
        "bpm": bpm_val,
        "lanes": [
            {"lane": 1, "key": "A"},
            {"lane": 2, "key": "S"},
            {"lane": 3, "key": "D"}
        ],
        "notes": notes
    }
    
    with open(output_json, 'w', encoding='utf-8') as f:
        json.dump(level_data, f, indent=2, ensure_ascii=False)
        
    normals = [n for n in notes if n["type"] == "normal"]
    holds = [n for n in notes if n["type"] == "hold"]
    traps = [n for n in notes if n["type"] == "trap"]
    
    print(f"\n📊 Итог идеального баланса:")
    print(f"  Всего нот: {len(notes)} | Обычные: {len(normals)} | Холды: {len(holds)} | Ловушки: {len(traps)}")
    print(f"💾 Карта сохранена в: {output_json}")

if __name__ == "__main__":
    generate_balanced_bj_map("track4.ogg", "example4.json")