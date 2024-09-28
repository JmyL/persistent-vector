# Task

In `task.cc` you will find the interface of a sample class `vector` as well as
a set of unit tests. Your task is to implement the functions defined in `vector`
so that they comply with the following requirements:

- the contents of `vector` are persisted to the directory given to the c'tor.
- the `vector` component recreates a previous state from the persistence directory
  when created
- the persistence schema must account for unexpected service shutdowns, like in the
  case of power outages
- the functions produce the same results as their STL counterparts, ie.
  `push_back` will append a string to the end of the `vector`
- all test cases shall pass

You may assume that:
- your data directory has unlimited disk space
- strings added to the vector are less than 4K long
- you can ignore RAM limitations


# Implementation

## Assumptions

1. 8 exabytes of storage and memory support is sufficient.
1. The filesystem used supports metadata journaling and files up to 8 exabytes (e.g. XFS).
1. Size of vector doesn't cross 2^64.
1. Accept zero length string as a valid item.
 

## Design

Store data as a binary log file with the format below.

`push_back` command:

|----------|--------|-----------|---- ... ----|
| **1**(8) |  Id(8) |  DSize(8) | Data(DSize) |

`erase` command:

|----------|--------|-----------|
| **2**(8) | RId(8) | RIndex(8) |

2. Id: Unique identifier for `push_back` command, used for debugging. This may be rotated.
3. DSize: Byte size of the data field.
4. Data: Data for a push command. For an erase command, it indicates the index to be removed.
4. RId: Id of command to be removed.
4. RIndex: Index to be removed.
4. Pad: Automatically added to align the next packet to an 8-byte boundary.

