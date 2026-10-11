# Arrays and standard Java wrappers

These operations run on the calling worker through an active
`vmhook::native_thread_context`, or inside a supported Java callback. They use
ordinary Java methods and the private VM call backend. No JNI, JVMTI, polling,
or deferred Java hook is needed to allocate arrays or mutate a collection.
Application wrappers must already be registered against their loaded classes.

Java references use owning `std::unique_ptr` wrappers. A Java-null reference
has a nonempty C++ wrapper with `get_instance() == nullptr`. An empty C++ owner
is invalid as an object argument. Returned objects and container elements retain
Java roots; they are not borrowed addresses into the collector's heap.

## Typed arrays

```cpp
const auto values = vmhook::make_array<std::int32_t>(3);
values->set(0, 42);
const auto first = values->get(0);
values->copy_from(std::array<std::int32_t, 3>{1, 2, 3});
const std::vector<std::int32_t> copied = values->to_vector();

const auto destination = vmhook::make_array<std::int32_t>(3);
destination->copy_from(values, 0, 0, values->size());
```

`array<T>` supports `bool`, `std::int8_t`, `std::uint16_t` for Java `char`,
`std::int16_t`, `std::int32_t`, `std::int64_t`, `float`, and `double`.
Primitive `to_vector()` copies the values. `copy_from(span, offset)` copies
primitive storage; boolean arrays also accept `std::vector<bool>`.
Java-to-Java `copy_from(source, source_offset, destination_offset, count)`
invokes `System.arraycopy` and supports reference arrays too.

```cpp
const auto first_player = vmhook::make_unique<player>(std::int32_t{1});
const auto java_null = std::make_unique<player>(nullptr);
const auto players = vmhook::make_array<player>(2);
players->set(0, first_player);
players->set(1, java_null);
const std::vector<std::unique_ptr<player>> snapshot = players->to_vector();

const auto rows = vmhook::make_array<vmhook::array<player>>(2);
rows->set(0, players);
const auto player_again = rows->get(0)->get(0);
```

Array allocation invokes `java.lang.reflect.Array.newInstance` so the JVM
allocates its own heap objects. Reference stores invoke `Array.set` so Java
checks component compatibility and applies the collector's write barriers.
Indices, primitive descriptors, array layouts, and storage widths are checked.
Accessing a Java-null array is an error; converting it to a vector yields an
empty snapshot. An incompatible element wrapper or a failed Java operation is
an explicit error. A snapshot does not track later Java changes.

## Collections, lists, sets, queues, and maps

Built-in wrappers include `collection`, `list`, `array_list`, `linked_list`,
`set`, `hash_set`, `linked_hash_set`, `tree_set`, `queue`, `deque`, `array_deque`,
`map`, `hash_map`, `linked_hash_map`, `tree_map`, and `concurrent_hash_map`.
Concrete wrappers can be constructed with `vmhook::make_unique<T>()`.

```cpp
const auto players = vmhook::make_unique<vmhook::array_list>();
players->add(first_player);
players->add(java_null);
const std::unique_ptr<player> previous = players->set(0, first_player);
const std::unique_ptr<player> selected = players->get(0);
const std::vector<std::unique_ptr<player>> snapshot = players->to_vector();

const std::unique_ptr<vmhook::iterator> cursor = players->iterator();
while (cursor->has_next())
{
    const std::unique_ptr<player> value = cursor->next();
    // Use value or cursor->remove() according to the Java iterator contract.
}

const auto by_player = vmhook::make_unique<vmhook::hash_map>();
const std::unique_ptr<player> old = by_player->put(first_player, java_null);
const std::unique_ptr<player> found = by_player->get(first_player);
const std::vector<std::pair<std::unique_ptr<player>, std::unique_ptr<player>>>
    entries = by_player->to_entries();
const auto copied_list = vmhook::make_collection(snapshot);
const auto copied_map = vmhook::make_map(entries);
```

`to_vector()` and `to_entries()` infer their C++ element types from the typed
return statement or initialization. Explicit template arguments are also
available. Erased Java collections cannot prove a generic element type before
their elements are inspected; incompatible wrapper conversions fail explicitly.

Collections offer `size`, `is_empty`, `contains`, `add`, `remove`, `clear`,
`add_all`, `contains_all`, `remove_all`, and `retain_all`. Lists also support
indexed `get`, `set`, `add`, `remove`, `index_of`, `last_index_of`, and `sub_list`.
Queues expose `offer`, `peek`, and `poll`; deques expose both ends. Maps offer
`get`, `put`, `remove`, `contains_key`, `contains_value`, `put_all`, `clear`,
`key_set`, `values`, and `entry_set`.

Mutations use the actual Java implementation, preserving its null restrictions,
ordering, comparator, concurrency rules, and exceptions. Iterators and sublists
are Java views, with the original fail-fast or weak-consistency semantics.
`to_vector()` is a snapshot, not an atomic transaction against concurrent
mutations. A map's Java-null result can mean either an absent key or a stored
null; use `contains_key()` when that distinction matters.

## Strings, boxed values, and Optional

```cpp
const auto text = vmhook::new_string<vmhook::java_string>("hello");
const std::string utf8 = text->to_string();
const std::u16string code_units = text->to_utf16();
const auto middle = text->substring(1, 4);

const auto number = vmhook::java_integer::value_of(42);
const auto value = number->value();
const auto present = vmhook::optional::of_nullable(first_player);
const std::unique_ptr<player> contained = present->get();
```

`java_string` offers `length`, `is_empty`, `char_at`, `substring`, `starts_with`,
and `ends_with`. Length and indices are UTF-16 code units, as in Java.
`to_utf16()` uses a rooted Java `char[]` copy and preserves unpaired surrogates;
`to_string()` follows VMHook's checked UTF-8 conversion rules.

`java_boolean`, `java_byte`, `java_char`, `java_short`, `java_integer`,
`java_long`, `java_float`, and `java_double` offer `value_of()` and `value()`.
`optional` offers `empty`, `of_nullable`, `is_present`, `is_empty`, `get`, and
`or_else`. These call the Java 8-compatible methods. `Optional.get()` on an
empty value raises the original Java failure.

`class_object` wraps `java.lang.Class`, including `name`, type predicates,
`loader`, `super_class`, `component_type`, and explicit-loader `for_name`.
`class_loader` offers `load_class` and `parent`. A bootstrap loader is an
allocated Java-null `class_loader` wrapper. `throwable` offers `message`,
`cause`, `to_string`, and `stack_trace`; `java_enum` offers `name` and `ordinal`.

## Copied NIO buffers

```cpp
const auto buffer = vmhook::byte_buffer::allocate_direct(4);
buffer->put(std::array<std::int8_t, 4>{-128, 0, 42, 127});
buffer->position(0);
const std::vector<std::int8_t> bytes = buffer->to_vector();
```

`byte_buffer` supports heap `allocate`, `allocate_direct`, `capacity`,
`is_direct`, `position`, `remaining`, indexed `get` and `put`, span `put`, and
`to_vector`. Its vector contains the remaining bytes. Conversion duplicates
the Java buffer so the caller's position stays unchanged. Span writes advance
the position according to Java's `put(byte[])` contract.

The API exposes copied C++ bytes, not a borrowed pointer to direct storage.
The retained Java buffer and its Cleaner own direct-memory lifetime. Read-only
buffers, invalid indices, and insufficient remaining space raise Java failures.
