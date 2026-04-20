# MapForGoblins -- база знаний

> English version: [KNOWLEDGE_EN.md](KNOWLEDGE_EN.md)

Всё что узнали за время разработки мода и исследования файлов Elden Ring Reforged.
Написано чтобы любой (человек или AI-агент) мог быстро войти в контекст.

---

## Что такое MapForGoblins

DLL-мод для Elden Ring Reforged (ERR). Добавляет ~9000 иконок на карту мира: оружие, броня, заклинания, квестовые предметы, боссы, NPC, Rune Pieces и т.д.

Ключевое отличие от обычного инсталлера -- мод **не трогает regulation.bin**, все данные инжектятся в память при загрузке DLL. Это позволяет играть онлайн без блокировки EAC (через Seamless Co-op / мод-лоадер).

Текущая версия: **v1.0.8** (pre), ~9000 записей WorldMapPointParam (+ ~740 ванильных), 60+ гранулярных категорий иконок в INI. Собранные Rune/Ember Pieces автоматически скрываются на карте.

---

## Архитектура DLL

### Модули

| Файл | Что делает |
|---|---|
| `dllmain.cpp` | Точка входа. Логгер (spdlog), загрузка конфига, запуск мод-потока |
| `goblin_inject.cpp` | Инжект записей в WorldMapPointParam (подмена ParamTable в памяти) |
| `goblin_messages.cpp` | Хук MsgRepositoryImp::LookupEntry для кастомного текста карты |
| `goblin_logic.cpp` | Логика map fragment -- иконки появляются только после сбора фрагмента карты |
| `goblin_collected.cpp` | Определение собранных Rune/Ember Pieces: GEOF (model hash + InstanceID slot) + WGM (+0x263 бит1 + +0x26B бит4) |
| `goblin_config.cpp` | Парсинг INI (mINI), 60+ переключателей категорий + debug_logging, парсинг VK-кода хоткея |
| `goblin_markers.cpp` | Опциональный дамп бикон-массивов из памяти по хоткею (отладочный, выключен по умолчанию) |
| `goblin_massedit.cpp` | Runtime-парсер MASSEDIT файлов (альтернативный путь загрузки из `dll/offline/massedit/`) |
| `generated/goblin_map_data.cpp` | Автосгенерированный массив из MASSEDIT файлов (~9000 записей) |
| `generated/goblin_legacy_conv.hpp` | Автосгенерированная таблица dungeon→overworld конверсии координат (из WorldMapLegacyConvParam) |
| `modutils.cpp` | AOB-сканер (Pattern16), хуки (MinHook), утилиты для работы с памятью |
| `from/params.cpp` | Работа с SoloParamRepository -- поиск и итерация по Param таблицам |

### Как работает инжект (goblin_inject.cpp)

1. Дождаться загрузки params (`from::params::initialize()`)
2. Найти `ParamResCap` для "WorldMapPointParam" в ParamList
3. Получить указатель на param_file через `rescap + 0x80`
4. `VirtualAlloc` новый буфер: header (0x40) + row locators + данные + type string + wrapper locators
5. Скопировать оригинальные строки + добавить наши, отсортировать по row_id
6. Атомарно подменить указатель: `file_ptr_ref = new_param_file`

Память ParamTable:
```
ParamResCap -> param_header (+0x78 = size, +0x80 = param_file ptr)
ParamTable (param_file):
  +0x00: param_type_offset (uint32)
  +0x0A: num_rows (uint16)
  +0x30: data_start (uint64)
  +0x40: ParamRowInfo[num_rows] -- по 24 байта: row_id(u64) + param_offset(u64) + param_end_offset(u64)
  [data_start..]: WORLD_MAP_POINT_PARAM_ST по 256 байт каждый
```

### Как работает текст (goblin_messages.cpp)

Хук на `MsgRepositoryImp::LookupEntry` (AOB: `48 8B 3D ?? ?? ?? ?? 44 0F B6 30 48 85 FF 75`).
Все наши PlaceName ID используют **offset-encoding** — никакого кастомного скомпилированного текста.
Старшие разряды ID кодируют, из какой существующей FMG-категории брать строку:

| Диапазон offset | Редирект в FMG-категорию |
|---|---|
| 100 000 000 + id | WeaponName |
| 200 000 000 + id | ProtectorName (броня) |
| 300 000 000 + id | AccessoryName (талисманы) |
| 400 000 000 + id | GemName (ashes of war) |
| 500 000 000 + id | GoodsName |
| 600 000 000 + id | Event text (tutorial/hint строки) |
| 900 000 000 + id | TutorialTitle |

Когда игра запрашивает PlaceName маркера, хук переводит offset обратно в исходный ID и
возвращает существующую в игре строку. Локализация во все 14 языковых слотов работает автоматически.
`goblin_text_data.cpp` больше не компилируется.

### Пайплайн генерации данных

Весь пайплайн оркеструется `tools/build_pipeline.py` (18 стадий, хеш-инкрементальный кеш
в `data/.build_cache.json`; холодный запуск ~240 с, полностью кешированный <1 с).

```
MSB файлы + regulation.bin + EMEVD
        |
        +-- extract_all_items.py    -->  items_database.json + классификация goods
        +-- build_entity_index.py   -->  msb_entity_index.json
        +-- scan_emevd_awards.py    -->  emevd_lot_mapping.json
        +-- enrich_fallback_with_emevd.py (in-place апгрейд неразрешённых записей)
        |
        +-- generate_loot_massedit.py    -->  50+ Loot/Equipment/Key/Quest/Magic MASSEDIT
        +-- generate_pieces_massedit.py  -->  Rune/Ember MASSEDIT + _slots.json
        +-- generate_material_nodes.py   -->  Loot - Material Nodes MASSEDIT
        +-- generate_graces.py, generate_summoning_pools.py, generate_spirit_springs.py,
        |   generate_imp_statues.py, generate_stakes.py, generate_paintings.py,
        |   generate_maps.py             -->  MASSEDIT мировой инфраструктуры
        +-- generate_gestures.py         -->  жесты (через сканирование common event 90005570)
        +-- generate_hostile_npcs.py     -->  инвейдеры (через NpcParam.teamType=24 + MSB)
        |
        v
  generate_data.py  -->  goblin_map_data.cpp + goblin_legacy_conv.hpp
                          (MapEntry.geom_slot вшит для каждого piece)
        |
        v
  CMake build  -->  MapForGoblins.dll
```

Парсинг MSB через Andre.SoulsFormats.dll (из Smithbox, копия в `tools/lib/`).
`MSBE.Read(string path)` через reflection — поддерживает и base game, и DLC карты.

---

## Ключевые структуры и форматы

### WORLD_MAP_POINT_PARAM_ST (256 байт)

Запись иконки на карте мира. Основные поля:

| Поле | Тип | Описание |
|---|---|---|
| iconId | int32 | ID иконки (376 = stonesword key стиль, 393 = стандартный лут) |
| posX, posZ | float | Координаты на карте (мировые координаты X и Z) |
| textId1 | int32 | ID текста PlaceName (наши начинаются с 9000000+) |
| textDisableFlagId1 | int32 | Event flag -- при активации иконка скрывается (подбор предмета) |
| eventFlagId | int32 | Флаг отображения (map fragment) |
| areaNo | int16 | Номер области (60 = overworld, 61 = DLC, 10-21 = подземелья) |
| gridXNo, gridZNo | int16 | Координаты тайла карты |
| dispMask00..07 | bits | Маски видимости слоёв карты |
| selectMinZoomStep | int32 | Минимальный зум для отображения |

### ItemLotParam_map

Определяет что лежит в конкретной "точке дропа". Связывается с MSB Treasure event.

- `lotItemId01..08` -- ID предмета (goods/weapon/armor/etc)
- `getItemFlagId` -- event flag, который ставится при подборе (для textDisableFlagId1)
- Row ID кодирует тайл карты: `AABBCCDDEE` -> area AA, grid BB_CC

### MSB (MSBE) -- файлы карт

Бинарные файлы уровней в `map/MapStudio/`. Содержат:
- **Parts** -- объекты в мире (Assets, Enemies, Players, DummyAssets, DummyEnemies, MapPieces, ConnectCollisions)
- Каждый Part имеет: Name, ModelName, Position (x,y,z), EntityID, EntityGroups[8], MapStudioLayer

Парсинг через `pythonnet` + `Andre.SoulsFormats.dll` (из Smithbox, копия в `tools/lib/`):
```python
from pythonnet import load
load('coreclr')
import clr
asm = Assembly.LoadFrom('tools/lib/Andre.SoulsFormats.dll')
# Andre.SoulsFormats: MSBE.Read(string path) через reflection:
_msbe_read_str = _msbe_type.GetMethod('Read', ..., Array[SysType]([str_type]), None)
msb = _msbe_read_str.Invoke(None, Array[Object]([path_to_msb_dcx]))
```

Andre.SoulsFormats поддерживает и base game, и DLC карты (DSMSPortable версия падает на DLC `WeatherOverride` region).

Каждый MSB Part имеет поле **InstanceID** - используется для GEOF slot маппинга:
```python
for asset in msb.Parts.Assets:
    instance_id = asset.InstanceID  # e.g. 9001
    geom_slot = instance_id - 9000  # e.g. 1
```

### EMEVD -- скомпилированные event-скрипты

Бинарные файлы событий в `event/`. Содержат:
- Events с уникальными ID
- Instructions с Bank:ID (напр. 2003:66 = SetEventFlag, 2000:00 = RunEvent)
- Аргументы в сыром виде (byte array), интерпретируются через EMEDF

Ключевые инструкции:
| Bank:ID | Название | Назначение |
|---|---|---|
| 2000:00 | RunEvent | Вызов вложенного события с аргументами |
| 2003:14 | WarpPlayer | Телепорт игрока (НЕ связано с gatherables) |
| 2003:22 | BatchSetEventFlags | Массовая установка флагов |
| 2003:36 | AwardItemsIncludingClients | Выдача предметов |
| 2003:66 | SetEventFlag | Установка одного флага |
| 2006:04 | CreateAssetFollowingSFX | Создание визуального эффекта |
| 2007:01 | DisplayGenericDialog | Показ диалогового окна |

### FMG -- текстовые файлы

Бинарный формат From Software для текстов. Версия 2 (Elden Ring):
- Header: version(u32), fileSize(u32), unk(u32), groupCount(u32)
- Groups по 16 байт: firstId(i32), lastId(i32), offsetsStart(i32)
- Строки в UTF-16LE

Хранятся внутри BND4 архивов (`item_dlc02.msgbnd.dcx`), сжатых DCX (zstd).

### DCX / BND4 / BHD5

- **DCX** -- контейнер сжатия (zstd для ER, magic `DCX\0`, zstd magic `\x28\xB5\x2F\xFD`)
- **BND4** -- архив файлов (MSB, FMG и др.)
- **BHD5** -- зашифрованный индекс архивов vanilla (Data0-3.bdt), ключ для EldenRing = Game enum value 3

---

## Rune Pieces и Ember Pieces -- полное исследование

Rune Pieces (и Ember Pieces в DLC) -- кастомные предметы ERR, разбросанные по миру. Маленькие жёлтые светящиеся камушки. При подборе дают Rune Piece (800010) и Runic Trace (800011) в инвентарь. Подобрать можно один раз за прохождение -- сохраняется в сейве.

### Идентификаторы

| Предмет | Goods ID | MSB модель | Кол-во | Где |
|---|---|---|---|---|
| Rune Piece | 800010 | AEG099_821 | 1164 | Base game (m10, m60, etc.) |
| Ember Piece | 850010 | AEG099_822 | 314 | DLC (m20, m21, m61) |
| Runic Trace | 800011 | -- | -- | Выдаётся вместе с Rune Piece |

### Модели в MSB

**AEG099_821** (Rune Piece):
- 1164 экземпляров по всем картам base game
- **96% (1028 шт.) имеют EntityID = 0** и пустые EntityGroups
- Только 41 штука имеют EntityID, причём всего 4 уникальных категориальных значения (напр. 1042610000)
- MapStudioLayer = 0xFFFFFFFF (все слои) у всех
- Dummy свойства: ReferenceID=100, Unk34=-1877326030; ReferenceID=90, Unk34=1075484236

**AEG099_822** (Ember Piece):
- 314 экземпляров в DLC картах
- Для парсинга DLC MSB нужна Andre.SoulsFormats.dll (из Smithbox), стандартная падает

**AEG099_510** ("якорные" объекты):
- 133 экземпляра
- Имеют уникальные EntityID
- Связаны с EMEVD событиями
- Только ~50 из них управляются через event 1045632900
- Не являются визуальными моделями кусочков -- это невидимые "триггеры" или "точки привязки"
- Не все AEG099_821 находятся рядом с AEG099_510

### EMEVD-цепочка (50 управляемых кусков)

```
Event 1045632900 (оркестратор, в common.emevd.dcx)
  |
  |-- RunEvent(1045630910, ...) × 50 раз
       |
       Аргументы:
         vals[3] = subEvent2 (collectedFlag, напр. 1045630100)
         vals[7] = subEvent4 (EntityID AEG099_510)
         vals[8] = lotId (ID из ItemLotParam_map)
         vals[9] = mapTile (закодирован как 4 LE-байта: mXX_YY_ZZ_00)
```

Event 1045630910 (обработчик одного куска):
1. Проверяет collectedFlag (subEvent2)
2. Если не собран -- создаёт SFX (2006:04), показывает диалог взаимодействия (2007:01)
3. При подборе -- SetEventFlag(2003:66) для subEvent2, AwardItemsIncludingClients(2003:36) для lotId
4. Скрывает объект

### Что удалось замапить

43 позиции полностью связаны: **lotId -> EntityID -> координаты XYZ -> event flag**

| Источник | Кол-во | Как |
|---|---|---|
| Event 1045632900 → AEG099_510 | 36 | subEvent4 = EntityID, subEvent2 = collectedFlag |
| Прямые entity_matches в MSB | 7 | EntityID совпадает с lot ID |

Данные в `data/_piece_final_map.json` и `data/_piece_complete_map.json`.

### РЕШЕНО: Tracking Rune/Ember Pieces

Полностью раскрыт через серию дампов памяти.

**Два источника данных:**

1. **GEOF синглтоны** (выгруженные тайлы):
   - GeomFlagSaveDataManager (RVA `0x3D69D18`) и GeomNonActiveBlockManager (RVA `0x3D69D98`)
   - Хранят записи ТОЛЬКО для уничтоженных/собранных объектов
   - Каждая запись 8 байт: flags, geom_idx, **model_hash** (байты 4-7)
   - Model hash `0x009A1C6D` = AEG099_821 (Rune Piece)
   - GEOF slot = `(geom_idx - 0x1194) * 2 + (flags >> 7)` = `InstanceID - 9000`

2. **CSWorldGeomMan** (загруженные тайлы, RVA `0x3D69BA8`) - **ПРИОРИТЕТ над GEOF**:
   - RB-tree загруженных блоков → geom_ins_vector → CSWorldGeomIns объекты
   - **Комбинированный флаг** (универсальный для AEG099_821/822/651/691):
     - +0x263 бит 1 (маска 0x02): постоянный, переживает рестарт
     - +0x26B бит 4 (маска 0x10): мгновенный после подбора, работает для всех типов моделей
   - `alive = (f263 & 0x02) && !(f26B & 0x10)` - жив только если ОБА флага согласны
   - WGM данные приоритетнее GEOF для загруженных тайлов (GEOF может быть устаревшим)
   - **Старый флаг `+0x269 & 0x60` устарел**: работает для 821/691, но НЕ для gathering
     nodes 651 (остаётся 0x10 после подбора). Заменён универсальным +0x26B бит 4.

**Ложный кандидат: +0x1D8** - processing state, не собранность. Мерцает при стриминге.

**GEOF slot маппинг:**
- Slot НЕ равен суффиксу имени (_9000 ≠ slot 0 на некоторых тайлах)
- Slot = `InstanceID - 9000` (InstanceID - поле MSB Part, читается через SoulsFormats)
- WGM маппинг: каждый piece привязан по `name_suffix` → `row_id` (не по позиции в векторе)

**Известные ограничения:**
- Хостинг Seamless Co-op крашится из-за VirtualAlloc'd ParamTable (старый баг, не связан с collected detection)

Детали: `geom_collection_tracking.md` в корне проекта.

---

## Оставшиеся задачи

### 1. Seamless Co-op хостинг
VirtualAlloc'd ParamTable (~9800 строк вместо 740) несовместима с Seamless Co-op при создании сессии (хостинг). Варианты:
- Хук param lookup вместо замены таблицы
- HeapAlloc вместо VirtualAlloc
- Разобраться что именно Seamless Co-op делает с param при хостинге

### 2. Справочные оффсеты

Позиция игрока:
```
WorldChrMan (RVA: base + 0x3D65F88)
  -> PlayerIns (+0x10EF8)
    -> ChrModules (+0x190)
      -> SubModule (+0xC0)
        -> WorldPosition (+0x40)  // float x, y, z
```

ERR-специфичные event ID:
- Event 1045632900 -- оркестратор Rune Pieces (50 RunEvent)
- Event 1045630910 -- обработчик одного куска с AEG099_510

---

## Зависимости и инструменты

### Для билда DLL (C++)
- CMake 3.28+, MSVC (Visual Studio Build Tools 2022)
- MinHook (хуки), Pattern16 (AOB сканер), mINI (INI парсер), spdlog (логгер)

### Для скриптов (Python)
- `pythonnet` -- вызов C#/.NET из Python (для SoulsFormats)
- `Andre.SoulsFormats.dll` (из Smithbox) -- парсер From Software форматов. Копия в `tools/lib/`.
  Поддерживает и base game, и DLC MSB (в отличие от DSMSPortable версии)
- `pymem` -- чтение памяти процесса (для дамп-скриптов)

### Пути
Все внешние пути настраиваются через `tools/config.ini` (скопировать из `config.ini.example`).
- ERR мод (`err_mod_dir`): папка с regulation.bin, map/, event/, msg/
- Игра (`game_dir`): папка с eldenring.exe и oo2core_6_win64.dll
- Andre.SoulsFormats.dll: `tools/lib/` (в репо)
- Paramdefs XML: `tools/paramdefs/` (в репо)

---

## Выходные данные (data/)

### JSON файлы
| Файл | Содержит |
|---|---|
| `items_database.json` | Все предметы из MSB Treasure + regulation.bin |
| `WorldMapPointParam.json` | Дамп ванильного WorldMapPointParam |
| `rune_pieces.json` | 1164 позиций AEG099_821 с InstanceID (после дедупа ~1113) |
| `ember_pieces.json` | 314 позиций AEG099_822 с InstanceID |
| `new_fmg_entries.json` | Новые текстовые записи для FMG |
| `comparison_report.json` | Сравнение MASSEDIT с items_database |

### Диагностические JSON (Rune Pieces исследование)
| Файл | Содержит |
|---|---|
| `_pieces_diagnostic.json` | Все rune/ember lot IDs, event flags, entity_matches |
| `_emevd_findings.json` | Попадания lot ID в EMEVD инструкциях |
| `_piece_mappings.json` | Координаты из per-map EMEVD |
| `_piece_complete_map.json` | 43 замапленных позиции (lotId+flag+coords) |
| `_piece_final_map.json` | Финальная карта с collectedFlag |
| `_piece_models.json` | Сравнение AEG099_510/821/822 |
| `_rune_pieces_821.json` | Полный анализ всех AEG099_821 (свойства, EntityID) |

---

## История решения проблем

### soulstruct не работает
Библиотека soulstruct (Python) сломана на Python 3.13 -- падает при импорте. Обход: pythonnet + SoulsFormats.dll напрямую.

### DLC MSB не парсятся
DSMSPortable SoulsFormats.dll падает на DLC MSB из-за `WeatherOverride` region. Фикс: Andre.SoulsFormats.dll из Smithbox, поддерживает DLC формат.

### MSBE.Read через pythonnet
Andre.SoulsFormats поддерживает `MSBE.Read(string path)` - читает и декомпрессирует DCX:
```python
_msbe_read_str = _msbe_type.GetMethod('Read', ..., Array[SysType]([str_type]), None)
msb = _msbe_read_str.Invoke(None, Array[Object]([path_to_msb_dcx]))
```

### EMEVD Instruction.Index -> Instruction.ID
В SoulsFormats класс `EMEVD.Instruction` использует `.ID` для номера инструкции (не `.Index`).

### dispMask / pad2_0 путаница
В MASSEDIT `pad2_0: = 1` соответствует `dispMask02` (бит 2 байта 0x18). Это DLC слой карты (area 61). Для overworld (area 60) используется `dispMask00`.

### Позиция игрока -- неверные оффсеты
ChrModules+0x68 (PhysMod)+0x70 давал (0,0,0). Правильная цепочка: ChrModules+0xC0 (SubModule)+0x40 дает реальные мировые координаты.

---

## Tracking Rune Pieces - РЕШЕНО

Подход: дамп памяти игры (Python + pymem) → побайтовое сравнение CSWorldGeomIns структур → комбинированный флаг (+0x263 persistent + +0x26B universal immediate) → верификация на 4+ дампах включая gathering nodes (AEG099_651).

Результат: все Rune/Ember Pieces корректно скрываются при подборе, как для загруженных, так и для выгруженных тайлов.

Детали: секция "РЕШЕНО: Tracking Rune/Ember Pieces" выше и `geom_collection_tracking.md`.
