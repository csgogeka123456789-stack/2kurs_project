import librosa
import numpy as np
import json
import random

def generate_beatmap(audio_file, output_json, lanes_count=3):
    print(f"Анализируем трек: {audio_file}...")
    
    # Загружаем аудиофайл
    y, sr = librosa.load(audio_file, sr=None)
    
    # Ищем темп (BPM) и удары (beats)
    tempo, beat_frames = librosa.beat.beat_track(y=y, sr=sr)
    
    # Получаем точное время каждого удара в секундах
    beat_times_sec = librosa.frames_to_time(beat_frames, sr=sr)
    
    notes = []
    
    # Проходим по всем найденным битам
    for time_sec in beat_times_sec:
        time_ms = int(time_sec * 1000)
        
        # Пропускаем первые 2 секунды (даем игроку приготовиться)
        if time_ms < 2000:
            continue
            
        # Случайно выбираем линию (от 1 до 3)
        lane = random.randint(1, lanes_count)
        
        # С вероятностью 85% это будет обычная нота, с 15% - ловушка
        note_type = "trap" if random.random() > 0.85 else "normal"
        
        notes.append({
            "time_ms": time_ms,
            "lane": lane,
            "type": note_type
        })
    
    # Собираем итоговый JSON
    level_data = {
        "song": audio_file,
        "bpm": int(tempo[0]) if isinstance(tempo, np.ndarray) else int(tempo),
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
        
    print(f"Успешно! Найдено нот: {len(notes)}. BPM: {level_data['bpm']}")
    print(f"Карта сохранена в: {output_json}")

# Запускаем скрипт для нашего файла
if __name__ == "__main__":
    generate_beatmap("track.ogg", "example.json")