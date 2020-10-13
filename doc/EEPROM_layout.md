EEPROM Layout
=============


| Idx   |   Content                        | Length  |
|-------|----------------------------------|---------|
|    1  | last track played in folder "01" | 1 Byte  |    
|   ..  | last track played in folder ".." | 1 Byte  | 
|   99  | last track played in folder "99" | 1 Byte  | 
|  100  | adminSettings                    | 34 Byte |