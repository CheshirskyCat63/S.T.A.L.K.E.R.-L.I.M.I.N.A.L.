# Native PhysX PBD ↔ IX-Ray — интерфейс интеграции

Статус: **план подключения**, ещё не внедрён в рабочую игру. Нативный GPU-PBD-прототип работает отдельно.

## 0. PBD — не «настройки PhysX»

PhysX содержит отдельный позиционно-ограниченный GPU-решатель PBD (`PxPBDParticleSystem`). Обычные твёрдые тела имеют другой контактный и интеграционный тракт. Изменение параметров ODE или обычного PhysX rigid-body не делает его PBD. Для PBD обязателен CUDA-совместимый GPU-режим.

## 1. Минимальный мост

На начальном этапе **ODE остаётся владельцем персонажей, NPC, оружейной баллистики и обычной статической коллизии**, а NVIDIA PhysX владеет только отдельным набором PBD-частиц. Не давать ODE и PhysX одновременно считать один и тот же игровой rigid body.

В `xrGame/Level_bullet_manager_firetrace.cpp` обрабатывать штатно уже **разрешённое** попадание, затем формировать событие `PhysicsImpactEvent`:

```cpp
struct PhysicsImpactEvent {
    float point[3];        // metres, IX-Ray world coordinates
    float normal[3];
    float velocity[3];     // actual projectile vector, not a preset FX direction
    float impulse_ns;      // verified actual impulse, not uncalibrated visual scale
    unsigned short game_mtl_index; // runtime GMLib index, resolve persistent ID separately
    unsigned int target_id;
    unsigned int sequence;
};
```

Игровой поток только кладёт `ImpactEvent` в bounded lock-free or single-producer queue. На физическом шаге мост переводит координаты и масштаб, вызывает `ovphysx` native C API, прикладывает воздействие к собственному PhysX-актору/системе и читает PBD-координаты. В первом варианте обмен **односторонний**: попадание возбуждает PBD; обратный импульс не меняет штатную баллистику/ODE. Для двусторонней связи понадобится явное владение телами и согласованный шаг.

## 2. Geom/material

- Отображать CFORM **не обязательно целиком** в CUDA VRAM: для частиц из активной области достаточно локальных подготовленных простых collision shapes.
- Не путать CFORM triangle material index, runtime GameMtlLib index и persistent ID.
- Первые контакты PBD/rigid — в отдельной тестовой сцене. Для карты нужен локальный трансформ с геометрией пола/препятствий.
- GameMtlLib остаётся каноническим источником материала; для PBD создаём таблицу дополнительных свойств (density, viscosity, cohesion, friction), не переписывая gamemtl.xr и не ломая акустику.

## 3. Rendering / GPU ownership

- При стартовом мосте PBD-частицы могут передаваться в CPU/readback buffer и отрисовываться штатными средствами R4.
- Zero-copy DLPack CUDA buffers **не означает** автоматической совместимости с DX11 рендером. Для прямого отображения понадобится защищённый interop или staged copy.
- Сами частицы — симуляция, а не готовый кинематографичный дым/огонь. У Flow отдельная газовая симуляция.

## 4. Runtime boundaries

- Python `.venv` нужен только для экспериментирования. **В игровой DLL не встраивать Python-процесс**.
- Native PhysX C API `ovphysx` (SDK в vendor/ovphysx-sdk) уже собран с Visual Studio и запускается. Из него делаем отдельную DLL-обёртку или native module IX-Ray, только после проверки binary ABI, зависимостей, lifetime и потока.
- Статическая карта и `level.cform` не пересобираются в событии выстрела.
- Для физического пролома стены нужен отдельный синхронизированный collision override; не считать PBD автоматически решателем CFORM-топологии.
- Следующий жёсткий критерий: сцена **в игровом IX-Ray** с 100–1000 PBD-гранул, 100 повторных выстрелов, без течи GPU/CPU, сравнение FPS/physics step, совместимость с аудио и движением.
- Отдельная внутренняя ветка `liminal/destruction`. Не менять аудио- и movement-ветки.

## 5. Не решено

Нет подключённого обработчика игровых выстрелов, PhysX particle material для устойчивой гравитационной кучи ещё не откалиброван, нет DX11 GPU interop и save/load PBD. Статус standalone-теста не равен игровому PASS.
