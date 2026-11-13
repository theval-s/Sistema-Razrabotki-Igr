# NavMesh и DetourCrowd

## Первый запуск в редакторе

1. Создай пол, например куб с масштабом `(20, 1, 20)` и позицией `(0, -0.5, 0)`. Верхняя грань будет на высоте `Y = 0`. Добавь **Rigid Body** с типом **Static**, **Collider → Box** и **Navigation Source**. Оставь **Use Collider** включённым.
2. В дереве сцены выбери **Add → NavMesh**. Поставь объект в `(0, 1, 0)`, укажи **Half extents** `(10, 3, 10)` и нажми **Bake Navigation Mesh**. В режиме редактирования бирюзовые линии покажут сетку. Рамка выбранного NavMesh идёт по мировым осям и перемещается вместе с объектом. Размер задаётся через **Half extents**.
3. В браузере ресурсов выбери **New → Navigation Follower Script**. Прикрепи созданный скрипт к кубу без динамического тела и поставь куб в `(-5, 0.5, 0)`. В **Script → Properties** задай `navMeshName = "NavMesh"`, `goalX = 5`, `goalY = 0`, `goalZ = 0` и `heightOffset = 0.5`.
4. Нажми **Play**. Куб пройдёт к цели. Чтобы проверить обход, добавь между ним и целью высокую стену со статическим коллайдером и **Navigation Source**, оставь проход по бокам и повтори Bake.

## Что попадает в сетку

| Настройка | Геометрия для запекания |
| --- | --- |
| **Navigation Source → Use Collider** включён | Статический коллайдер. Удобно для пола, стен и крупных препятствий. |
| **Navigation Source → Use Collider** выключен | Загруженный меш из **Mesh Renderer**. Удобно для импортированного уровня. |

В область NavMesh должна попадать проходимая геометрия хотя бы одного источника. Достаточно, чтобы область пересекала геометрию, даже если центр объекта находится снаружи. Триггеры, динамические и кинематические тела, а также объекты без **Navigation Source** исключаются. Объекту, который будет двигаться по сетке, этот компонент не нужен.

**Agent radius** задаёт зазор от препятствий, **Agent height** — нужную высоту прохода. **Maximum step** и **Maximum slope** ограничивают ступени и уклоны. Размеры ячейки определяют точность и стоимость запекания. Если сетка не появилась, проверь границы, источник пола и размеры агента. Если меш источника ещё грузится, дождись загрузки и повтори Bake.

## Несколько NavMesh и Lua

Каждый объект NavMesh запекается отдельно и может иметь свой размер агента. Сохраняются объекты, параметры и источники. Запечённые сетки хранятся в памяти. После открытия сцены или изменения геометрии повтори Bake. При входе в Play доступны текущие сетки редактора.

В Lua укажи нужный объект по уникальному имени. Точки задаются в мировых координатах на уровне пола.

```lua
local mesh = Scene.FindByName("NavMesh")
local points = Navigation.FindPath(Vector3.new(-5, 0, 0), Vector3.new(5, 0, 0), mesh)
if #points == 0 then
    Log.warning("Маршрут не найден")
end
```

Результат — точки полного маршрута на одной сетке. Без третьего аргумента движок выберет кратчайший из найденных полных маршрутов. Маршруты между разными сетками пока не строятся.

## Движение толпы

**Navigation Follower Script** идёт по точкам маршрута. Для взаимного обхода зарегистрируй объекты в одной сетке через **DetourCrowd**. Следующий скрипт можно вставить в обычный Lua-ассет и прикрепить к нескольким объектам. Размести их в разных стартовых точках и задай разные цели в инспекторе.

```lua
Properties = {
    navMeshName = "NavMesh",
    speed = 3,
    heightOffset = 0.5,
    goalX = 5,
    goalY = 0,
    goalZ = 0,
}

local mesh
local registered = false

function OnStart()
    mesh = Scene.FindByName(Properties.navMeshName)
    local feet = entity:GetWorldPosition()
    feet.y = feet.y - Properties.heightOffset
    registered = Navigation.RegisterAgent(mesh, entity, feet, Properties.speed)
    if not registered then
        Log.warning("Проверь имя NavMesh, запекание и стартовую точку")
        return
    end
    local goal = Vector3.new(Properties.goalX, Properties.goalY, Properties.goalZ)
    if not Navigation.SetAgentGoal(mesh, entity, goal) then
        Log.warning("Цель не принята")
    end
end

function OnUpdate()
    if not registered then return end
    local feet = Navigation.GetAgentPosition(mesh, entity)
    if feet then
        feet.y = feet.y + Properties.heightOffset
        entity:SetWorldPosition(feet)
    end
end

function OnDestroy()
    if registered then Navigation.RemoveAgent(mesh, entity) end
end
```

DetourCrowd поддерживает до 128 агентов на сетку и использует высоту и положительный радиус из её настроек. `SetAgentGoal` сообщает, принят ли запрос. Успешный запрос не гарантирует прибытия. Stop очищает агентов.
