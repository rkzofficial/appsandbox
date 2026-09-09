/*
 * vz_vm -- Wraps VZVirtualMachine with load/start/stop helpers.
 */

#import <Foundation/Foundation.h>
#import <Virtualization/Virtualization.h>

NS_ASSUME_NONNULL_BEGIN

typedef void (^VzVmStateChangeBlock)(VZVirtualMachineState state);

/* Headless: omit the VM's audio devices entirely on the next load. Host mic
 * capture would block on a TCC consent prompt no daemon can show, and output
 * would play on the host speakers with no window to gate it. Set by
 * asb_mac_set_headless before any VM start. */
void vz_vm_set_no_audio(BOOL no_audio);

@interface VzVm : NSObject

@property (nonatomic, strong, readonly) VZVirtualMachine *machine;
@property (nonatomic, strong, readonly) NSString *name;
@property (nonatomic, copy, nullable) VzVmStateChangeBlock onStateChange;

/* Build a VZVirtualMachineConfiguration from on-disk state for the named VM.
 * Returns a ready-to-start wrapper, or nil on error. */
+ (nullable VzVm *)loadVmNamed:(NSString *)name
                         ramMb:(int)ramMb
                      cpuCores:(int)cpuCores
                         error:(NSError **)error;

/* Same, with the initial guest display size in pixels (the core passes the VM's
 * configured mode, defaulting to 2560x1600 for macOS guests; 0 = that default).
 * VZVirtualMachineView.automaticallyReconfiguresDisplay then follows the window,
 * so this is the mode the guest boots with. VZ exposes no refresh rate. */
+ (nullable VzVm *)loadVmNamed:(NSString *)name
                         ramMb:(int)ramMb
                      cpuCores:(int)cpuCores
                  displayWidth:(int)displayWidth
                 displayHeight:(int)displayHeight
                         error:(NSError **)error;

/* Build a fresh configuration for install (no disks are loaded from disk.img;
 * this is used by vz_install during VZMacOSInstaller setup). */
+ (nullable VZVirtualMachineConfiguration *)buildInstallConfigurationForName:(NSString *)name
                                                      hardwareModel:(VZMacHardwareModel *)hardwareModel
                                                   auxiliaryStorage:(VZMacAuxiliaryStorage *)aux
                                                  machineIdentifier:(VZMacMachineIdentifier *)machineId
                                                              ramMb:(int)ramMb
                                                           cpuCount:(int)cpuCount
                                                              error:(NSError **)error;

- (void)startWithCompletion:(void (^)(NSError * _Nullable))completion;
- (void)stopWithCompletion:(void (^)(NSError * _Nullable))completion;
- (void)requestStopWithCompletion:(void (^)(NSError * _Nullable))completion;

@end

NS_ASSUME_NONNULL_END
