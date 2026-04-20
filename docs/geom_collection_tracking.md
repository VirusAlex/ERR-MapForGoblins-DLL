# How to Detect Collected Geom Objects in Elden Ring (from Process Memory)

## Reading Geom Object State from Elden Ring Memory in Real Time

**Game version:** Elden Ring 1.16 (Calibrations 1.16.1)  
**Context:** Research was done for the Elden Ring Reforged (ERR) mod, but the mechanism is vanilla engine stuff.  
**Authors:** VirusAlex + Claude (Anthropic)  
**Date:** April 2026

---

## The Problem

Mods for Elden Ring sometimes need to know whether a geom object (CSWorldGeomIns, aka AEG object - a resource, item, interactive object) has been picked up or destroyed by the player. For example, ERR scatters 1000+ Rune Pieces (AEG099_821) across the map, and we need to hide their map icons after collection.

The catch: "collected" state lives in different places depending on whether the tile is currently loaded or not.

## Architecture: Two Data Sources

The game manages geom objects through two systems that alternate:

### 1. GEOF Singletons (Unloaded Tiles)

When a tile is **not loaded** (far from the player), info about collected objects on that tile lives in GEOF tables:

- **GeomFlagSaveDataManager** - RVA `0x3D69D18` from `eldenring.exe` base
- **GeomNonActiveBlockManager** - RVA `0x3D69D98`

Both are singleton pointers. Dereference the pointer, get a table:

```
[ptr + 0x08]: table start
Each element: 16 bytes
  [+0x00] tile_id : u64   (upper 32 bits usually 0)
  [+0x08] data_ptr : u64  → pointer to tile data block
```

Tile data block (at data_ptr):
```
Layout A (GeomFlagSaveDataManager):
  [+0x08] count : u32
  [+0x10] entries[] : 8 bytes each

Layout B (GeomNonActiveBlockManager):
  [+0x00] count : u32
  [+0x08] entries[] : 8 bytes each
```

Each entry - 8 bytes:
```
  [+0x00] byte0 : u8     (always 0x01)
  [+0x01] flags : u8     (0x00 or 0x80)
  [+0x02] geom_idx : u16 (little-endian)
  [+0x04] model_hash : u32 (little-endian, identifies the asset model)
```

**GEOF only stores COLLECTED/DESTROYED objects** - alive objects have no entries. If a tile has 5 geom objects and 2 are collected, GEOF has 2 entries.

### 2. CSWorldGeomMan / WGM (Loaded Tiles)

When a tile is **loaded** (near the player), its GEOF data disappears from the singletons. Objects become live CSWorldGeomIns instances in memory.

**CSWorldGeomMan** - RVA `0x3D69BA8`

Structure:
```
WGM → [+0x18] RB-tree: { +0x08 head_ptr, +0x10 size }

Tree node:
  [+0x00] left
  [+0x08] parent
  [+0x10] right
  [+0x18] color (1 byte)
  [+0x19] is_nil (1 byte, 0 = not nil)
  [+0x20] block_id : u32
  [+0x28] block_data_ptr : u64

BlockData:
  [+0x288] geom_ins_vector: { +0x08 begin, +0x10 end }

Each vector element is a pointer to CSWorldGeomIns (8 bytes)
```

To identify a specific object, read its name. The sub-structures are embedded inline (offsets add up), not pointer-chained:
```
CSWorldGeomIns + 0x18 + 0x18 + 0x18 = +0x48 → *msb_part_ptr → *name_ptr (wchar_t*)

In detail:
  CSWorldGeomIns
    [+0x18] CSWorldGeomInfo (inline)
      [+0x18] CSMsbPartsGeom (inline)
        [+0x18] msb_part_ptr  → deref → MsbPart
          [+0x00] name_ptr    → deref → wchar_t[] name
```

Example names: `AEG099_821_9000`, `AEG099_821_9001`, ...

### The "Spawn Tile" Problem

Tiles near the player's spawn point load **directly from the save file into WGM**, bypassing GEOF singletons entirely. They never appear in GeomFlagSaveDataManager or GeomNonActiveBlockManager until the player walks far enough away.

This means for ~5% of tiles (nearest to the player on load), there is no GEOF data. You have to determine "collected" status from the CSWorldGeomIns objects themselves.

## The Collected Flag: Combined Check (+0x263 + +0x26B)

### Three Flags Found

By comparing memory dumps byte by byte, we scanned **the first 0x300 (768) bytes** of CSWorldGeomIns across **4 dumps** (spawn, walked, restart+teleport, another restart) - 32 samples total with known ground truth. Later expanded to cover gathering nodes (AEG099_651) which use a different immediate flag.

**Flag 1: +0x263 bit 1** - persistent, survives game restarts
```
Address:  CSWorldGeomIns + 0x263
Mask:     0x02 (bit 1)

  bit SET (0x02)  → object is ALIVE (not collected)
  bit CLR (0x00)  → object is COLLECTED / destroyed
```

Typical byte values: `0x7E`/`0x7F` = alive, `0x7C`/`0x7D` = collected.

**Flag 2: +0x26B bit 4** - UNIVERSAL immediate flag, all model types (lost on tile reload)
```
Address:  CSWorldGeomIns + 0x26B
Mask:     0x10 (bit 4)

  0x06 (bit CLR) → ALIVE
  0x16 (bit SET) → COLLECTED (set immediately on pickup)
```

**Flag 3: +0x269 & 0x60** - immediate, but ONLY for destructible pickups (821/691), NOT gathering nodes (651)
```
Address:  CSWorldGeomIns + 0x269
Mask:     0x60 (bits 5 and 6)

  0x00 → ALIVE (or gathering node — this flag does NOT work for 651!)
  0x60 → COLLECTED (only set for rune pieces 821, ember pieces 822, and one-time nodes like 691)
```

### Which Flag To Use

| Object type | +0x263 (persistent) | +0x26B (immediate) | +0x269 (immediate) |
|-------------|--------------------|--------------------|-------------------|
| AEG099_821 (Rune Pieces) | Yes (slow) | **Yes** | Yes |
| AEG099_822 (Ember Pieces) | Yes (slow) | **Yes** | Yes |
| AEG099_691 (Trina's Lily etc.) | Yes (slow) | **Yes** | Yes |
| AEG099_651 (Erdleaf etc.) | **Never updates while tile loaded!** | **Yes** | **NO** |

**Use +0x26B, not +0x269.** The +0x269 flag was discovered first on AEG099_821 and appeared sufficient, but it does not trigger for AEG099_651 gathering nodes. The +0x26B flag is universal.

### Recommended Check

```
alive = (byte_263 & 0x02) != 0  AND  (byte_26B & 0x10) == 0
```

- **+0x263** catches objects collected in previous sessions (persists in save, restored on tile load)
- **+0x26B** catches objects collected just now while the tile is loaded (immediate)
- Both flags reset to their "alive" state when a tile unloads and reloads — but +0x263 gets restored from save data with the correct "dead" state

### AEG099_651 Caveat

For gathering nodes (AEG099_651), +0x263 does **not** update while the tile is loaded. It only reflects the correct state after the tile unloads (GEOF saves it) and reloads. This means:
- Immediate detection relies **solely** on +0x26B for 651 nodes
- After tile reload, +0x263 is correct (0x7D = dead)

### Verification

- **+0x263 bit 1**: 32/32 correct across 4 dumps including restarts (821 only)
- **+0x26B bit 4**: verified on 3x AEG099_651 + 1x AEG099_821, before/after same object comparison
- **+0x269 & 0x60**: works for 821/691 but NOT for 651 (verified: stays 0x10 after 651 collection)

### WGM Priority Over GEOF

For tiles loaded in WGM (near the player), WGM data always takes priority over GEOF. GEOF may have stale data (e.g., a piece collected moments ago won't be in GEOF yet). Only use GEOF for tiles NOT currently loaded in WGM.

### False Lead: +0x1D8

Many reversers check `+0x1D8` (processing/destruction state) first. This is a **trap**:
- Values 0/2/3 are **processing state**, not collection status
- On the current tile (where the player stands) it happens to match real status
- On neighboring tiles it **lies**: a live object can show 2, a collected one can show 0
- Fluctuates during tile streaming

## GEOF Entry Filtering: Model Hash

GEOF entries contain ALL destroyed geom objects on a tile - not just the ones you care about. To find entries for a specific model (e.g. AEG099_821 Rune Pieces), use the **model hash** at bytes 4-7 of each entry:

```
Model hash for AEG099_821: 0x009A1C6D (bytes: 6D 1C 9A 00)
Model hash for AEG099_822: (extract from MSB for your version)
```

This hash matches the value at CSWorldGeomIns +0x28 for live objects of that model.

Filter: only process GEOF entries where `entry[4..7] == model_hash`.

## GEOF Slot → Piece Mapping via InstanceID

Each GEOF entry encodes a **slot number**:
```
slot = (geom_idx - 0x1194) * 2 + ((flags & 0x80) ? 1 : 0)
```

This slot does **NOT** directly map to the piece name suffix (_9000, _9001...). The slot corresponds to the MSB Part's **InstanceID**:

```
slot = InstanceID - 9000
```

InstanceID is a field on each MSB Part, readable via SoulsFormats. On most tiles, InstanceID matches the name suffix (so _9000 has InstanceID=9000 -> slot=0). But on some tiles there are offsets - e.g., InstanceID=9002 for _9001, giving slot=2 instead of 1.

**You must read InstanceID from MSB data** (via SoulsFormats or SmithBox) to build the correct slot-to-piece mapping. Do not assume `slot = name_suffix - 9000`.

## Tile ID (BlockId)

Tile ID encoding:
```
tile_id = (area << 24) | (gridX << 16) | (gridZ << 8) | index

Example: m60_33_44_00 → area=60(0x3C), gridX=33(0x21), gridZ=44(0x2C), idx=0
         → tile_id = 0x3C212C00
```

## Code Example (C++)

```cpp
// Check if a CSWorldGeomIns object has been collected
// Combined: +0x263 bit1 (persistent) + +0x26B bit4 (universal immediate)
// Alive only if BOTH flags agree it's alive
bool is_geom_collected(void* geom_ins) {
    uint8_t f263 = 0, f26B = 0;
    safe_read((char*)geom_ins + 0x263, &f263, 1);
    safe_read((char*)geom_ins + 0x26B, &f26B, 1);
    bool alive = (f263 & 0x02) && !(f26B & 0x10);
    return !alive;
}
```

## Code Example (Python, via pymem)

```python
import pymem

pm = pymem.Pymem("eldenring.exe")

def is_geom_collected(geom_ins_addr):
    """Check if a GeomIns object has been collected.
    Combined: +0x263 (persistent) + +0x26B (universal immediate)."""
    f263 = pm.read_uchar(geom_ins_addr + 0x263)
    f26B = pm.read_uchar(geom_ins_addr + 0x26B)
    alive = (f263 & 0x02) and not (f26B & 0x10)
    return not alive
```

## Overview

```
Player loads a save
         │
         ▼
┌──────────────────────┐
│  Tiles far from      │──► GEOF singletons (RVA 0x3D69D18, 0x3D69D98)
│  player (95%)        │    Filter entries by model_hash (bytes 4-7)
│                      │    slot = (geom_idx - 0x1194) * 2 + (flags >> 7)
│                      │    Map slot to piece via InstanceID from MSB
└──────────────────────┘
         │
┌──────────────────────┐
│  Tiles near          │──► CSWorldGeomMan (RVA 0x3D69BA8) ← PRIORITY
│  player (5%)         │    RB-tree → BlockData → geom_ins_vector
│                      │    Combined: +0x263 & 0x02 (persistent)
│                      │            + +0x26B & 0x10 (universal immediate)
└──────────────────────┘
```

---
---

# Как определить, собран ли геом-объект в Elden Ring (из памяти процесса)

## Как читать состояние Geom-объектов из памяти Elden Ring в реальном времени

**Версия игры:** Elden Ring 1.16 (Calibrations 1.16.1)  
**Контекст:** Исследование проводилось для мода Elden Ring Reforged (ERR), но механизм ванильный, из движка игры.  
**Авторы:** VirusAlex + Claude (Anthropic)  
**Дата:** Апрель 2026

---

## Проблема

В Elden Ring модам может потребоваться определить, был ли геом-объект (CSWorldGeomIns, он же AEG-объект - ресурс, предмет, интерактивный объект) подобран/уничтожен игроком. Например, мод ERR раскидывает 1000+ осколков рун (AEG099_821) по миру, и нужно скрывать их иконки на карте после сбора.

Загвоздка: "собранность" хранится по-разному в зависимости от того, загружен тайл или нет.

## Архитектура: два источника данных

Игра управляет геом-объектами через две системы, работающие поочерёдно:

### 1. GEOF-синглтоны (выгруженные тайлы)

Когда тайл **не загружен** в мир (далеко от игрока), информация о собранных на нём объектах хранится в GEOF-таблицах:

- **GeomFlagSaveDataManager** - RVA `0x3D69D18` от базы `eldenring.exe`
- **GeomNonActiveBlockManager** - RVA `0x3D69D98`

Оба - синглтон-указатели. Разыменовываешь указатель, получаешь таблицу вида:

```
[ptr + 0x08]: начало таблицы
Каждый элемент: 16 байт
  [+0x00] tile_id : u64   (верхние 32 бита обычно 0)
  [+0x08] data_ptr : u64  → указатель на блок данных тайла
```

Блок данных тайла (по data_ptr):
```
Layout A (GeomFlagSaveDataManager):
  [+0x08] count : u32
  [+0x10] entries[] : 8 байт каждая

Layout B (GeomNonActiveBlockManager):
  [+0x00] count : u32
  [+0x08] entries[] : 8 байт каждая
```

Каждая entry - 8 байт:
```
  [+0x00] byte0 : u8     (всегда 0x01)
  [+0x01] flags : u8     (0x00 или 0x80)
  [+0x02] geom_idx : u16 (little-endian)
  [+0x04] model_hash : u32 (little-endian, идентифицирует модель ассета)
```

**GEOF хранит только СОБРАННЫЕ/УНИЧТОЖЕННЫЕ объекты** - живые объекты не имеют записей. Если на тайле 5 геом-объектов и 2 собраны, в GEOF будет 2 записи.

### 2. CSWorldGeomMan / WGM (загруженные тайлы)

Когда тайл **загружен** (рядом с игроком), его GEOF-данные исчезают из синглтонов. Объекты становятся живыми CSWorldGeomIns в памяти.

**CSWorldGeomMan** - RVA `0x3D69BA8`

Структура:
```
WGM → [+0x18] RB-tree: { +0x08 head_ptr, +0x10 size }

Узел дерева:
  [+0x00] left
  [+0x08] parent
  [+0x10] right
  [+0x18] color (1 байт)
  [+0x19] is_nil (1 байт, 0 = не nil)
  [+0x20] block_id : u32
  [+0x28] block_data_ptr : u64

BlockData:
  [+0x288] geom_ins_vector: { +0x08 begin, +0x10 end }

Каждый элемент вектора - указатель на CSWorldGeomIns (8 байт)
```

Чтобы найти конкретный объект, читаем его имя. Внутренние структуры встроены inline (смещения складываются), а не через цепочку указателей:
```
CSWorldGeomIns + 0x18 + 0x18 + 0x18 = +0x48 → *msb_part_ptr → *name_ptr (wchar_t*)

Подробно:
  CSWorldGeomIns
    [+0x18] CSWorldGeomInfo (inline)
      [+0x18] CSMsbPartsGeom (inline)
        [+0x18] msb_part_ptr  → разыменовываем → MsbPart
          [+0x00] name_ptr    → разыменовываем → wchar_t[] имя
```

Примеры имён: `AEG099_821_9000`, `AEG099_821_9001`, ...

### Проблема "spawn-тайлов"

Тайлы, находящиеся рядом с точкой спавна игрока, загружаются **напрямую из сейва в WGM**, минуя GEOF-синглтоны. Они никогда не попадают в GeomFlagSaveDataManager и GeomNonActiveBlockManager до тех пор, пока игрок не отойдёт от них достаточно далеко.

Это значит, что для ~5% тайлов (ближайших к игроку при загрузке) GEOF-данных нет, и нужно определять "собранность" по самим CSWorldGeomIns-объектам.

## Флаг собранности: комбинированная проверка (+0x263 + +0x26B)

### Три найденных флага

Побайтовым сравнением дампов памяти перебрали **первые 0x300 (768) байт** CSWorldGeomIns на **4 дампах** (spawn, walked, restart+teleport, ещё один restart) - 32 сэмпла с известным ground truth. Позже расширили исследование на gathering nodes (AEG099_651), которые используют другой immediate-флаг.

**Флаг 1: +0x263 бит 1** - постоянный, переживает рестарт
```
Адрес:  CSWorldGeomIns + 0x263
Маска:  0x02 (бит 1)

  бит УСТАНОВЛЕН (0x02) → объект ЖИВ (не собран)
  бит СНЯТ (0x00)       → объект СОБРАН / уничтожен
```

Типичные значения: `0x7E`/`0x7F` = жив, `0x7C`/`0x7D` = собран.

**Флаг 2: +0x26B бит 4** - УНИВЕРСАЛЬНЫЙ мгновенный флаг, все типы моделей (сбрасывается при перезагрузке тайла)
```
Адрес:  CSWorldGeomIns + 0x26B
Маска:  0x10 (бит 4)

  0x06 (бит СНЯТ) → ЖИВ
  0x16 (бит УСТАНОВЛЕН) → СОБРАН (устанавливается мгновенно при подборе)
```

**Флаг 3: +0x269 & 0x60** - мгновенный, но ТОЛЬКО для уничтожаемых подборов (821/691), НЕ для gathering nodes (651)
```
Адрес:  CSWorldGeomIns + 0x269
Маска:  0x60 (биты 5 и 6)

  0x00 → ЖИВ (или gathering node — этот флаг НЕ работает для 651!)
  0x60 → СОБРАН (только для rune pieces 821, ember pieces 822, one-time nodes вроде 691)
```

### Какой флаг использовать

| Тип объекта | +0x263 (persistent) | +0x26B (immediate) | +0x269 (immediate) |
|-------------|--------------------|--------------------|-------------------|
| AEG099_821 (Rune Pieces) | Да (медленно) | **Да** | Да |
| AEG099_822 (Ember Pieces) | Да (медленно) | **Да** | Да |
| AEG099_691 (Лилия Трины и др.) | Да (медленно) | **Да** | Да |
| AEG099_651 (Лист Эрдрева и др.) | **Не обновляется пока тайл загружен!** | **Да** | **НЕТ** |

**Используйте +0x26B, а не +0x269.** Флаг +0x269 был обнаружен первым на AEG099_821 и казался достаточным, но он не срабатывает для gathering nodes AEG099_651. Флаг +0x26B универсален.

### Рекомендуемая проверка

```
alive = (byte_263 & 0x02) != 0  AND  (byte_26B & 0x10) == 0
```

- **+0x263** ловит объекты, собранные в предыдущих сессиях (сохраняется в сейве, восстанавливается при загрузке тайла)
- **+0x26B** ловит объекты, собранные прямо сейчас пока тайл загружен (мгновенно)
- Оба флага сбрасываются в "живое" состояние при выгрузке/загрузке тайла — но +0x263 восстанавливается из сейва с правильным "мёртвым" состоянием

### Особенность AEG099_651

Для gathering nodes (AEG099_651) флаг +0x263 **не обновляется** пока тайл загружен. Он отражает правильное состояние только после выгрузки тайла (GEOF сохраняет) и повторной загрузки:
- Мгновенная детекция полностью зависит от +0x26B для 651 нод
- После перезагрузки тайла +0x263 корректен (0x7D = мёртв)

### Проверка

- **+0x263 бит 1**: 32/32 корректно на 4 дампах включая рестарты (только 821)
- **+0x26B бит 4**: проверено на 3x AEG099_651 + 1x AEG099_821, сравнение before/after одного и того же объекта
- **+0x269 & 0x60**: работает для 821/691, но НЕ для 651 (проверено: остаётся 0x10 после сбора 651)

### Приоритет WGM над GEOF

Для тайлов загруженных в WGM (рядом с игроком), данные WGM всегда приоритетнее GEOF. GEOF может содержать устаревшие данные (например, осколок подобранный секунду назад ещё не в GEOF). GEOF используется только для тайлов НЕ загруженных в WGM.

### Ложный кандидат: +0x1D8

Многие реверсеры первым делом смотрят на `+0x1D8` (processing/destruction state). Это **ловушка**:
- Значение 0/2/3 - это **состояние обработки**, а не собранности
- На текущем тайле (где стоит игрок) совпадает с реальным статусом
- На соседних тайлах **врёт**: живой объект может показывать 2, собранный - 0
- При стриминге тайлов значение мерцает

## Фильтрация GEOF записей: хеш модели

GEOF содержит записи обо ВСЕХ уничтоженных геом-объектах на тайле - не только тех, которые нужны. Чтобы найти записи для конкретной модели (например AEG099_821 Rune Pieces), используйте **хеш модели** из байтов 4-7 каждой записи:

```
Хеш модели AEG099_821: 0x009A1C6D (байты: 6D 1C 9A 00)
Хеш модели AEG099_822: (извлеките из MSB для вашей версии)
```

Этот хеш совпадает со значением по смещению CSWorldGeomIns +0x28 для живых объектов этой модели.

Фильтр: обрабатывать только GEOF записи, где `entry[4..7] == model_hash`.

## GEOF slot → маппинг на осколок через InstanceID

Каждая GEOF запись кодирует **номер слота**:
```
slot = (geom_idx - 0x1194) * 2 + ((flags & 0x80) ? 1 : 0)
```

Этот слот **НЕ** совпадает напрямую с суффиксом имени (_9000, _9001...). Слот соответствует **InstanceID** из MSB Part:

```
slot = InstanceID - 9000
```

InstanceID - поле каждого MSB Part, читаемое через SoulsFormats. На большинстве тайлов InstanceID совпадает с суффиксом имени (_9000 имеет InstanceID=9000 -> slot=0). Но на некоторых тайлах есть сдвиг - например, InstanceID=9002 для _9001, что даёт slot=2 вместо 1.

**Нужно читать InstanceID из MSB данных** (через SoulsFormats или SmithBox) для корректного маппинга slot → piece. Нельзя считать, что `slot = суффикс_имени - 9000`.

## Tile ID (BlockId)

Tile ID кодируется как:
```
tile_id = (area << 24) | (gridX << 16) | (gridZ << 8) | index

Пример: m60_33_44_00 → area=60(0x3C), gridX=33(0x21), gridZ=44(0x2C), idx=0
         → tile_id = 0x3C212C00
```

## Пример кода (C++)

```cpp
// Проверка "собран ли объект" для CSWorldGeomIns
// Комбинированная: +0x263 бит1 (постоянный) + +0x26B бит4 (универсальный мгновенный)
// Жив только если ОБА флага согласны
bool is_geom_collected(void* geom_ins) {
    uint8_t f263 = 0, f26B = 0;
    safe_read((char*)geom_ins + 0x263, &f263, 1);
    safe_read((char*)geom_ins + 0x26B, &f26B, 1);
    bool alive = (f263 & 0x02) && !(f26B & 0x10);
    return !alive;
}
```

## Пример кода (Python, через pymem)

```python
import pymem

pm = pymem.Pymem("eldenring.exe")

def is_geom_collected(geom_ins_addr):
    """Проверяет, собран ли GeomIns объект.
    Комбинация: +0x263 (постоянный) + +0x26B (универсальный мгновенный)."""
    f263 = pm.read_uchar(geom_ins_addr + 0x263)
    f26B = pm.read_uchar(geom_ins_addr + 0x26B)
    alive = (f263 & 0x02) and not (f26B & 0x10)
    return not alive
```

## Итоговая схема

```
Игрок загружает сейв
         │
         ▼
┌──────────────────────┐
│  Тайлы далеко от     │──► GEOF синглтоны (RVA 0x3D69D18, 0x3D69D98)
│  игрока (95%)        │    Фильтр записей по model_hash (байты 4-7)
│                      │    slot = (geom_idx - 0x1194) * 2 + (flags >> 7)
│                      │    Маппинг slot → piece через InstanceID из MSB
└──────────────────────┘
         │
┌──────────────────────┐
│  Тайлы рядом с       │──► CSWorldGeomMan (RVA 0x3D69BA8) ← ПРИОРИТЕТ
│  игроком (5%)        │    RB-tree → BlockData → geom_ins_vector
│                      │    Комбинация: +0x263 & 0x02 (постоянный)
│                      │              + +0x26B & 0x10 (универсальный мгновенный)
└──────────────────────┘
```
