/*
 * Copyright (c) 2007, 2008 University of Tsukuba
 * Copyright (C) 2007, 2008
 *      National Institute of Information and Communications Technology
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 * 3. Neither the name of the University of Tsukuba nor the names of its
 *    contributors may be used to endorse or promote products derived from
 *    this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#ifdef CRYPTO_VPN
#ifdef VPN_VE
#include "config.h"
#include "cpu_mmu.h"
#include "crypt.h"
#include "current.h"
#include "initfunc.h"
#include "printf.h"
#include "spinlock.h"
#include "string.h"
#include "vmmcall.h"
#include "vpn_ve.h"
#include "vpnsys.h"

// NIC
struct VPN_NIC
{
	VPN_CTX *VpnCtx;					// コンテキスト
	SE_NICINFO NicInfo;					// NIC 情報
	SE_SYS_CALLBACK_RECV_NIC *RecvCallback;	// パケット受信時のコールバック
	void *RecvCallbackParam;			// コールバックパラメータ
	SE_QUEUE *SendPacketQueue;			// 送信パケットキュー
	SE_QUEUE *RecvPacketQueue;			// 受信パケットキュー
	bool IsVirtual;						// 仮想 NIC かどうか
	SE_LOCK *Lock;						// ロック
};
// VPN クライアントコンテキスト
struct VPN_CTX
{
	SE_HANDLE VpnClientHandle;			// VPN クライアントハンドル
	VPN_NIC *PhysicalNic;				// 物理 NIC
	SE_HANDLE PhysicalNicHandle;		// 物理 NIC ハンドル
	VPN_NIC *VirtualNic;				// 仮想 NIC
	SE_HANDLE VirtualNicHandle;			// 仮想 NIC ハンドル
	SE_LOCK *LogQueueLock;				// ログのキューのロック
	SE_QUEUE *LogQueue;					// ログのキュー
};

void crypt_sys_log(char *type, char *message);
void crypt_sys_get_physical_nic_info(SE_HANDLE nic_handle, SE_NICINFO *info);
void crypt_sys_send_physical_nic(SE_HANDLE nic_handle, UINT num_packets, void **packets, UINT *packet_sizes);
void crypt_sys_set_physical_nic_recv_callback(SE_HANDLE nic_handle, SE_SYS_CALLBACK_RECV_NIC *callback, void *param);
void crypt_sys_get_virtual_nic_info(SE_HANDLE nic_handle, SE_NICINFO *info);
void crypt_sys_send_virtual_nic(SE_HANDLE nic_handle, UINT num_packets, void **packets, UINT *packet_sizes);
void crypt_sys_set_virtual_nic_recv_callback(SE_HANDLE nic_handle, SE_SYS_CALLBACK_RECV_NIC *callback, void *param);
void crypt_nic_recv_packet(VPN_NIC *n, UINT num_packets, void **packets, UINT *packet_sizes);
VPN_NIC *crypt_init_physical_nic(VPN_CTX *ctx);
VPN_NIC *crypt_init_virtual_nic(VPN_CTX *ctx);
void crypt_flush_old_packet_from_queue(SE_QUEUE *q);

static VPN_CTX *vpn_ctx = NULL;			// VPN クライアントコンテキスト

// ve (Virtual Ethernet)
static spinlock_t ve_lock;

// Virtual Ethernet (ve) メイン処理
void crypt_ve_main(UCHAR *in, UCHAR *out)
{
	VE_CTL *cin, *cout;
	VPN_NIC *nic = NULL;
	// 引数チェック
	if (in == NULL || out == NULL)
	{
		return;
	}

	cin = (VE_CTL *)in;
	cout = (VE_CTL *)out;

	memset(cout, 0, sizeof(VE_CTL));

	if (cin->EthernetType == VE_TYPE_PHYSICAL)
	{
		// 物理的な LAN カード
		nic = vpn_ctx->PhysicalNic;
	}
	else if (cin->EthernetType == VE_TYPE_VIRTUAL)
	{
		// 仮想的な LAN カード
		nic = vpn_ctx->VirtualNic;
	}

	if (nic != NULL)
	{
		if (cin->Operation == VE_OP_GET_LOG)
		{
			// ログから 1 行取得
			SeLock(vpn_ctx->LogQueueLock);
			{
				char *str = SeGetNext(vpn_ctx->LogQueue);

				if (str != NULL)
				{
					cout->PacketSize = SeStrSize(str);
					SeStrCpy((char *)cout->PacketData, sizeof(cout->PacketData), str);

					SeFree(str);
				}

				cout->NumQueue = vpn_ctx->LogQueue->num_item;

				cout->RetValue = 1;
			}
			SeUnlock(vpn_ctx->LogQueueLock);
		}
		else if (cin->Operation == VE_OP_GET_NEXT_SEND_PACKET)
		{
			// 次に送信すべきパケットの取得 (vpn -> vmm -> guest)
			SeLock(nic->Lock);
			{
				// 古いパケットがある場合は破棄
				crypt_flush_old_packet_from_queue(nic->SendPacketQueue);

				// パケットが 1 つ以上キューにあるかどうか
				if (nic->SendPacketQueue->num_item >= 1)
				{
					// キューから次のパケットをとる
					void *packet_data = SeGetNext(nic->SendPacketQueue);
					UINT packet_data_size = SeMemSize(packet_data);
					UINT packet_size_real = packet_data_size - sizeof(UINT64);
					void *packet_data_real = ((UCHAR *)packet_data) + sizeof(UINT64);

					memcpy(cout->PacketData, packet_data_real, packet_size_real);
					cout->PacketSize = packet_size_real;

					cout->NumQueue = nic->SendPacketQueue->num_item;

					// メモリ解放
					SeFree(packet_data);
				}

				cout->RetValue = 1;
			}
			SeUnlock(nic->Lock);
		}
		else if (cin->Operation == VE_OP_PUT_RECV_PACKET)
		{
			bool flush = false;
			UINT num_packets = 0;
			void **packets = NULL;
			UINT *packet_sizes = NULL;

			// 受信したパケットの書き込み (guest -> vmm -> vpn)
			SeLock(nic->Lock);
			{
				// 受信パケットは、パフォーマンス向上のため
				// すぐに vpn に渡さずにいったん受信キューにためる
				void *packet_data;
				UINT packet_size = cin->PacketSize;

				if (packet_size >= 1)
				{
					packet_data = SeClone(cin->PacketData, packet_size);

					SeInsertQueue(nic->RecvPacketQueue, packet_data);
				}

				if (cin->NumQueue == 0)
				{
					// もうこれ以上受信パケットが無い場合は
					// flush する (vpn に一気に渡す)
					flush = true;
				}

				cout->RetValue = 1;

				if (flush)
				{
					UINT i;
					void *p;

					num_packets = nic->RecvPacketQueue->num_item;
					packets = SeMalloc(sizeof(void *) * num_packets);
					packet_sizes = SeMalloc(sizeof(UINT *) * num_packets);

					i = 0;

					while (true)
					{
						UINT size;
						p = SeGetNext(nic->RecvPacketQueue);
						if (p == NULL)
						{
							break;
						}

						size = SeMemSize(p);

						packets[i] = p;
						packet_sizes[i] = size;

						i++;
					}
				}
			}
			SeUnlock(nic->Lock);

			if (flush)
			{
				UINT i;

				crypt_nic_recv_packet(nic, num_packets, packets, packet_sizes);

				for (i = 0;i < num_packets;i++)
				{
					SeFree(packets[i]);
				}

				SeFree(packets);
				SeFree(packet_sizes);
			}
		}
	}
}

// Virtual Ethernet (ve) ハンドラ
void crypt_ve_handler()
{
	UINT i;
	UCHAR data[VE_BUFSIZE];
	UCHAR data2[VE_BUFSIZE];
	intptr addr;
	bool ok = true;

	if (!config.vmm.driver.vpn.ve)
		return;
	spinlock_lock(&ve_lock);

	current->vmctl.read_general_reg(GENERAL_REG_RBX, &addr);

	for (i = 0;i < (VE_BUFSIZE / 4);i++)
	{
		if (read_linearaddr_l((u32)((ulong)(((UCHAR *)addr) + (i * 4))), (u32 *)(data + (i * 4))) != VMMERR_SUCCESS)
		{
			ok = false;
			break;
		}
	}
	
#ifndef	CRYPTO_VPN
	ok = false;
#endif	// CRYPTO_VPN

	if (ok == false)
	{
		current->vmctl.write_general_reg(GENERAL_REG_RAX, 0);

		spinlock_unlock(&ve_lock);
		return;
	}

	crypt_ve_main(data, data2);

	for (i = 0;i < (VE_BUFSIZE / 4);i++)
	{
		if (write_linearaddr_l((u32)((ulong)(((UCHAR *)addr) + (i * 4))), *((UINT *)(&data2[i * 4]))) != VMMERR_SUCCESS)
		{
			ok = false;
			break;
		}
	}

	if (ok == false)
	{
		current->vmctl.write_general_reg(GENERAL_REG_RAX, 0);

		spinlock_unlock(&ve_lock);
		return;
	}

	current->vmctl.write_general_reg(GENERAL_REG_RAX, 1);

	spinlock_unlock(&ve_lock);
}

// Virtual Ethernet (ve) の初期化
void crypt_ve_init()
{
	printf("Initing Virtual Ethernet (VE) for VPN Client Module...\n");
	spinlock_init(&ve_lock);
	vmmcall_register("ve", crypt_ve_handler);
	printf("Virtual Ethernet (VE): Ok.\n");
}

INITFUNC ("vmmcal9", crypt_ve_init);

// 物理 NIC の初期化
VPN_NIC *crypt_init_physical_nic(VPN_CTX *ctx)
{
	VPN_NIC *n = SeZeroMalloc(sizeof(VPN_NIC));

	SeStrToBinEx(n->NicInfo.MacAddress, sizeof(n->NicInfo.MacAddress),
		"00:12:12:12:12:12");
	n->NicInfo.MediaSpeed = 1000000000;
	n->NicInfo.MediaType = SE_MEDIA_TYPE_ETHERNET;
	n->NicInfo.Mtu = 1500;

	n->RecvPacketQueue = SeNewQueue();
	n->SendPacketQueue = SeNewQueue();

	n->VpnCtx = ctx;

	n->IsVirtual = false;

	n->Lock = SeNewLock();

	return n;
}

// 仮想 NIC の作成
VPN_NIC *crypt_init_virtual_nic(VPN_CTX *ctx)
{
	VPN_NIC *n = SeZeroMalloc(sizeof(VPN_NIC));

	SeStrToBinEx(n->NicInfo.MacAddress, sizeof(n->NicInfo.MacAddress),
		"00:AC:AC:AC:AC:AC");
	n->NicInfo.MediaSpeed = 1000000000;
	n->NicInfo.MediaType = SE_MEDIA_TYPE_ETHERNET;
	n->NicInfo.Mtu = 1500;

	n->RecvPacketQueue = SeNewQueue();
	n->SendPacketQueue = SeNewQueue();

	n->VpnCtx = ctx;

	n->IsVirtual = true;

	n->Lock = SeNewLock();

	return n;
}

static struct nicfunc vefunc = {
	.GetPhysicalNicInfo = crypt_sys_get_physical_nic_info,
	.SendPhysicalNic = crypt_sys_send_physical_nic,
	.SetPhysicalNicRecvCallback = crypt_sys_set_physical_nic_recv_callback,
	.GetVirtualNicInfo = crypt_sys_get_virtual_nic_info,
	.SendVirtualNic = crypt_sys_send_virtual_nic,
	.SetVirtualNicRecvCallback = crypt_sys_set_virtual_nic_recv_callback,
};

// VPN クライアントの初期化
void crypt_init_vpn()
{
	vpn_ctx = SeZeroMalloc(sizeof(VPN_CTX));

	// ログ用キューの初期化
	vpn_ctx->LogQueue = SeNewQueue();
	vpn_ctx->LogQueueLock = SeNewLock();

	// 物理 NIC の作成
	vpn_ctx->PhysicalNic = crypt_init_physical_nic(vpn_ctx);
	vpn_ctx->PhysicalNicHandle = (SE_HANDLE)vpn_ctx->PhysicalNic;

	// 仮想 NIC の作成
	vpn_ctx->VirtualNic = crypt_init_virtual_nic(vpn_ctx);
	vpn_ctx->VirtualNicHandle = (SE_HANDLE)vpn_ctx->VirtualNic;

	// VPN Client の作成
	//vpn_ctx->VpnClientHandle = VPN_IPsec_Client_Start(vpn_ctx->PhysicalNicHandle, vpn_ctx->VirtualNicHandle, "config.txt");
	vpn_ctx->VpnClientHandle = vpn_new_nic (vpn_ctx->PhysicalNicHandle,
						vpn_ctx->VirtualNicHandle,
						&vefunc);
}

// 提供システムコール: ログの出力 (画面に表示)
void crypt_sys_log(char *type, char *message)
{
	char *lf = "\n";
	void crypt_add_log_queue(char *type, char *message);

	if (message[strlen(message) - 1] == '\n')
	{
		lf = "";
	}

	if (type != NULL && SeStrLen(type) >= 1)
	{
		printf("%s: %s%s", type, message, lf);
	}
	else
	{
		printf("%s%s", message, lf);
	}

	crypt_add_log_queue(type, message);
}

// ログキューにログデータを追加
void crypt_add_log_queue(char *type, char *message)
{
	char *tmp;
	char tmp2[512];
	// 引数チェック
	if (type == NULL || message == NULL)
	{
		return;
	}
	if (vpn_ctx == NULL)
	{
		return;
	}

	tmp = SeCopyStr(message);
	if (SeStrLen(tmp) >= 1)
	{
		if (tmp[SeStrLen(tmp) - 1] == '\n')
		{
			tmp[SeStrLen(tmp) - 1] = 0;
		}
	}
	if (SeStrLen(tmp) >= 1)
	{
		if (tmp[SeStrLen(tmp) - 1] == '\r')
		{
			tmp[SeStrLen(tmp) - 1] = 0;
		}
	}

	if (type != NULL && SeStrLen(type) >= 1)
	{
		SeFormat(tmp2, sizeof(tmp2), "%s: %s", type, tmp);
	}
	else
	{
		SeStrCpy(tmp2, sizeof(tmp2), tmp);
	}

	SeFree(tmp);

	SeLock(vpn_ctx->LogQueueLock);
	{
		while (vpn_ctx->LogQueue->num_item > CRYPT_MAX_LOG_QUEUE_LINES)
		{
			char *p = SeGetNext(vpn_ctx->LogQueue);

			SeFree(p);
		}

		SeInsertQueue(vpn_ctx->LogQueue, SeCopyStr(tmp2));
	}
	SeUnlock(vpn_ctx->LogQueueLock);
}

// 提供システムコール: 物理 NIC の情報の取得
void crypt_sys_get_physical_nic_info(SE_HANDLE nic_handle, SE_NICINFO *info)
{
	VPN_NIC	*n = (VPN_NIC *)nic_handle;
	// 引数チェック
	if (n == NULL)
	{
		return;
	}

	SeCopy(info, &n->NicInfo, sizeof(SE_NICINFO));
}

// 送信パケットキューから古いパケットを削除する
void crypt_flush_old_packet_from_queue(SE_QUEUE *q)
{
	UINT64 now = SeTick64();
	UINT num = 0;

	while (true)
	{
		void *data = SePeekNext(q);
		UINT64 *time_stamp;

		if (data == NULL)
		{
			break;
		}

		time_stamp = (UINT64 *)data;

		if (now <= ((*time_stamp) + CRYPT_SEND_PACKET_LIFETIME))
		{
			break;
		}

		data = SeGetNext(q);

		SeFree(data);

		num++;
	}
}

// 提供システムコール: 物理 NIC を用いてパケットを送信
void crypt_sys_send_physical_nic(SE_HANDLE nic_handle, UINT num_packets, void **packets, UINT *packet_sizes)
{
	VPN_NIC	*n = (VPN_NIC *)nic_handle;
	UINT i;
	// 引数チェック
	if (n == NULL || num_packets == 0 || packets == NULL || packet_sizes == NULL)
	{
		return;
	}

	SeLock(n->Lock);
	{
		// パケットをキューに格納する
		for (i = 0;i < num_packets;i++)
		{
			void *packet = packets[i];
			UINT size = packet_sizes[i];

			UCHAR *packet_copy = SeMalloc(size + sizeof(UINT64));
			SeCopy(packet_copy + sizeof(UINT64), packet, size);
			*((UINT64 *)packet_copy) = SeTick64();

			SeInsertQueue(n->SendPacketQueue, packet_copy);
		}

		// 古いパケットを送信キューから削除する
		crypt_flush_old_packet_from_queue(n->SendPacketQueue);
	}
	SeUnlock(n->Lock);
}

// 提供システムコール: 物理 NIC からパケットを受信した際のコールバックを設定
void crypt_sys_set_physical_nic_recv_callback(SE_HANDLE nic_handle, SE_SYS_CALLBACK_RECV_NIC *callback, void *param)
{
	VPN_NIC	*n = (VPN_NIC *)nic_handle;
	// 引数チェック
	if (n == NULL)
	{
		return;
	}

	n->RecvCallback = callback;
	n->RecvCallbackParam = param;
}

// 提供システムコール: 仮想 NIC の情報の取得
void crypt_sys_get_virtual_nic_info(SE_HANDLE nic_handle, SE_NICINFO *info)
{
	VPN_NIC	*n = (VPN_NIC *)nic_handle;

	SeCopy(info, &n->NicInfo, sizeof(SE_NICINFO));
}

// 提供システムコール: 仮想 NIC を用いてパケットを送信
void crypt_sys_send_virtual_nic(SE_HANDLE nic_handle, UINT num_packets, void **packets, UINT *packet_sizes)
{
	VPN_NIC	*n = (VPN_NIC *)nic_handle;
	UINT i;
	// 引数チェック
	if (n == NULL || num_packets == 0 || packets == NULL || packet_sizes == NULL)
	{
		return;
	}

	SeLock(n->Lock);
	{
		// パケットをキューに格納する
		for (i = 0;i < num_packets;i++)
		{
			void *packet = packets[i];
			UINT size = packet_sizes[i];

			UCHAR *packet_copy = SeMalloc(size + sizeof(UINT64));
			SeCopy(packet_copy + sizeof(UINT64), packet, size);
			*((UINT64 *)packet_copy) = SeTick64();

			SeInsertQueue(n->SendPacketQueue, packet_copy);
		}

		// 古いパケットを送信キューから削除する
		crypt_flush_old_packet_from_queue(n->SendPacketQueue);
	}
	SeUnlock(n->Lock);
}

// 提供システムコール: 仮想 NIC からパケットを受信した際のコールバックを設定
void crypt_sys_set_virtual_nic_recv_callback(SE_HANDLE nic_handle, SE_SYS_CALLBACK_RECV_NIC *callback, void *param)
{
	VPN_NIC	*n = (VPN_NIC *)nic_handle;
	// 引数チェック
	if (n == NULL)
	{
		return;
	}

	n->RecvCallback = callback;
	n->RecvCallbackParam = param;
}

// 物理 / 仮想 NIC でパケットを受信したことを通知しパケットデータを渡す
void crypt_nic_recv_packet(VPN_NIC *n, UINT num_packets, void **packets, UINT *packet_sizes)
{
	// 引数チェック
	if (n == NULL || num_packets == 0 || packets == NULL || packet_sizes == NULL)
	{
		return;
	}

	n->RecvCallback((SE_HANDLE)n, num_packets, packets, packet_sizes, n->RecvCallbackParam);
}

static void
vpn_ve_init (void)
{
	if (!config.vmm.driver.vpn.ve)
		return;
	crypt_init_vpn ();
}

INITFUNC ("driver1", vpn_ve_init);
#endif /* VPN_VE */
#endif /* CRYPTO_VPN */
