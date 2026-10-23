import librosa
import numpy as np
import json
import random

def generate_beatmap(audio_file, output_json, lanes_count=3):
    print(f"--- Начало анализа: {audio_file} ---")
    
    # 1. Загрузка файла
    y, sr = librosa.load(audio_file, sr=None)
    
    # 2. Анализ ритма (BPM и позиции ударов)
    # Используем onset_strength для более четкого определения битов в агрессивной музыке
    onset_env = librosa.onset.onset_strength(y=y, sr=sr)
    tempo, beat_frames = librosa.beat.beat_track(onset_envelope=onset_env, sr=sr)
    beat_times_sec = librosa.frames_to_time(beat_frames, sr=sr)
    
    # 3. Анализ громкости (RMS) для определения протяжности звуков
    rms = librosa.feature.rms(y=y)[0]
    if max(rms) > 0:
        rms = rms / max(rms)  # Нормализация громкости трека к 0.0 - 1.0
    
    notes = []
    
    # ПАРАМЕТРЫ НАСТРОЙКИ (идеальны для быстрых треков)
    HOLD_CHANCE = 0.20       # 20% шанс, что длинный звук станет холдом
    MIN_HOLD_MS = 200        # Минимальная длина зажатия
    ENERGY_THRESHOLD = 0.70  # Чем выше, тем строже отбор холдов (0.70 = 70% от пика удара)
    TRAP_CHANCE = 0.10       # 10% шанс появления ловушки
    GAP_BETWEEN_NOTES = 60   # Зазор в мс между концом холда и следующей нотой
    
    # 4. Проход по всем битам
    for i in range(len(beat_frames)):
        time_ms = int(beat_times_sec[i] * 1000)
        
        # Пропускаем вступление (первые 2 секунды)
        if time_ms < 2000:
            continue
            
        current_frame = min(beat_frames[i], len(rms) - 1)
        next_frame = beat_frames[i+1] if i + 1 < len(beat_frames) else len(rms) - 1
        
        # Считаем длительность "хвоста" звука
        start_energy = rms[current_frame]
        threshold = start_energy * ENERGY_THRESHOLD
        
        end_frame = current_frame
        for f in range(current_frame + 1, next_frame):
            if rms[f] < threshold:
                break
            end_frame = f
            
        detected_duration = int(librosa.frames_to_time([end_frame - current_frame], sr=sr)[0] * 1000)
        lane = random.randint(1, lanes_count)
        
        # 5. Логика выбора типа ноты
        is_hold = detected_duration >= MIN_HOLD_MS and random.random() < HOLD_CHANCE
        
        if is_hold:
            # Вычисляем макс. возможную длину до следующей ноты с учетом зазора
            ms_until_next = int(librosa.frames_to_time([next_frame - current_frame], sr=sr)[0] * 1000)
            final_duration = min(detected_duration, ms_until_next - GAP_BETWEEN_NOTES)
            
            # Если после обрезки холд все еще адекватной длины
            if final_duration >= 150:
                notes.append({
                    "time_ms": time_ms,
                    "lane": lane,
                    "type": "hold",
                    "duration_ms": final_duration
                })
                continue # Переходим к следующему биту

        # Если не холд, создаем обычную ноту или ловушку
        note_type = "trap" if random.random() < TRAP_CHANCE else "normal"
        notes.append({
            "time_ms": time_ms,
            "lane": lane,
            "type": note_type
        })
    
    # 6. Формирование и сохранение JSON
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
    
    with open(output_json, 'w', encoding='utf-8') as f:
        json.dump(level_data, f, indent=2, ensure_ascii=False)
        
    # Статистика в консоль
    h_count = len([n for n in notes if n["type"] == "hold"])
    t_count = len([n for n in notes if n["type"] == "trap"])
    print(f"Успех! Сгенерировано нот: {len(notes)}")
    print(f"Холды: {h_count}, Ловушки: {t_count}, Обычные: {len(notes)-h_count-t_count}")
    print(f"Файл сохранен: {output_json}")

if __name__ == "__main__":
    # Укажи имя своего файла здесь
    generate_beatmap("track3.ogg", "example3.json")