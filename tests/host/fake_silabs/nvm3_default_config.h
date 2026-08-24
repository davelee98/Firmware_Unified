#ifndef OD_TEST_FAKE_SILABS_NVM3_DEFAULT_CONFIG_H
#define OD_TEST_FAKE_SILABS_NVM3_DEFAULT_CONFIG_H

/* Mirrors the BG22 target's NVM3_MAX_OBJECT_SIZE=2112 compile definition, so the storage file's
 * `OD_CONFIG_MAX_SIZE + 16 > NVM3_DEFAULT_MAX_OBJECT_SIZE` guard is exercised as it is on target. */
#define NVM3_DEFAULT_MAX_OBJECT_SIZE 2112

#endif /* OD_TEST_FAKE_SILABS_NVM3_DEFAULT_CONFIG_H */
