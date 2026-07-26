# Panel backend for this board. Read by main/CMakeLists.txt in BOTH of IDF's cmake passes,
# so it must contain nothing but plain set() -- no cache variables, no IDF_TARGET, no
# includes. See the comment at its include site for why.
set(OD_USE_FASTEPD ON)
