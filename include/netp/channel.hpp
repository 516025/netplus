#ifndef _NETP_CHANNEL_HPP
#define _NETP_CHANNEL_HPP

#include <netp/core.hpp>
#include <netp/packet.hpp>
#include <netp/io_event_loop.hpp>
#include <netp/address.hpp>

#include <netp/promise.hpp>
#include <netp/channel_pipeline.hpp>

//CHANNEL STAGE
//1, dialing and pipeline initialize stage, we'll get a future notifIcation for any failure in this stage
//2, protocol handshake stage, we'll fire a error to all pipe if we failed 
//3, connected until closed stage, we can do read, write during these stages, no error would be fired
 
namespace netp {

	enum class channel_flag {
		F_WATCH_READ =1<<0, //watch read
		F_READ_ERROR = 1<<1,
		F_READ_SHUTDOWNING = 1<<2, //op boundary flag
		F_READ_SHUTDOWN = 1<<3,

		F_WATCH_WRITE = 1 << 4, //watch write
		F_WRITE_ERROR = 1 << 5, 
		F_WRITE_SHUTDOWNING = 1 << 6, //op boundary flag
		F_WRITE_SHUTDOWN = 1 << 7,

		F_READWRITE_SHUTDOWN = (int(channel_flag::F_READ_SHUTDOWN) | int(channel_flag::F_WRITE_SHUTDOWN)),

		F_WRITE_BARRIER = 1<<8,
		F_WRITING = 1<<9, //directly write flag, used by mux_stream and direct socket write
		F_WRITE_SHUTDOWN_PENDING = 1<<10,
		F_FIN_RECEIVED = 1 << 11,

		F_CLOSE_PENDING = 1<<12, //for transport, update close state
		F_CLOSING = 1 << 13,
		F_CLOSED = 1 << 14,

		F_CONNECTING =1<<16,
		F_CONNECTED = 1<<17,
		F_LISTENING = 1<<18,

		F_ACTIVE = 1 << 20,
		F_BDLIMIT = 1 << 21,
		F_BDLIMIT_TIMER = 1 << 22,

		F_IO_EVENT_LOOP_BEGINING = 1<<24,
		F_IO_EVENT_LOOP_BEGIN_DONE = 1<<25,
		F_IO_EVENT_LOOP_BEGIN_FAILED = 1<<26,
		F_IO_EVENT_LOOP_NOTIFY_TERMINATING=1<<27,

		F_TIMER_1 = 1 << 28
	};

	struct channel_buf_cfg {
		u32_t rcv_size;
		u32_t snd_size;
	};

	enum channel_buf_range {
		CH_BUF_RCV_MAX_SIZE = (83388608U),//1024 * 1024 * 8,
		CH_BUF_RCV_MIN_SIZE = (8192U),
		CH_BUF_SND_MAX_SIZE = (83388608U),//1024 * 1024 * 8,
		CH_BUF_SND_MIN_SIZE = (8192U)
	};

	class channel_pipeline;
	typedef SOCKET channel_id_t;

	class channel;
	typedef std::function<void(NRP<channel>const& ch)> fn_channel_initializer_t;

	typedef netp::promise<std::tuple<int, NRP<netp::channel>>> channel_dial_promise;
	typedef netp::promise<std::tuple<int, NRP<netp::channel>>> channel_listen_promise;

	//NOTE: block|write_unblock hass been removed 
	//the writer would get notified by write_promise->set when packet write done
	//if write order is important , writer should maintain it's own write queue
	class channel :
		public netp::ref_base
	{

	public:
		NRP<io_event_loop> L;
	protected:
		int m_chflag;
		int m_cherrno;
	private:
		NRP<channel_pipeline> m_pipeline;
		NRP<promise<int>> m_ch_close_p;
		NRP<ref_base>	m_ctx;

	protected:
		#define CH_FIRE_ACTION_IMPL_PACKET_1(_NAME, _IN) \
			__NETP_FORCE_INLINE void ch_fire_##_NAME(NRP<packet> const& _IN) const { \
				m_pipeline->fire_##_NAME(_IN); \
			} \

			CH_FIRE_ACTION_IMPL_PACKET_1(read, income)

	#define CH_FIRE_ACTION_IMPL_PACKET_ADDR(_NAME,_IN,_ADDR) \
			__NETP_FORCE_INLINE void ch_fire_##_NAME(NRP<packet> const& _IN, address const& _ADDR) const { \
				m_pipeline->fire_##_NAME(_IN, _ADDR); \
			} \

			CH_FIRE_ACTION_IMPL_PACKET_ADDR(readfrom, income, from)

	#define CH_FIRE_ACTION_IMPL_0(_NAME) \
			__NETP_FORCE_INLINE void ch_fire_##_NAME() const  { \
				m_pipeline->fire_##_NAME(); \
			} \

			//NOTE: there is no pair(connected, closed) gurantee for UDP channel
			CH_FIRE_ACTION_IMPL_0(connected)
			CH_FIRE_ACTION_IMPL_0(read_closed)
			CH_FIRE_ACTION_IMPL_0(write_closed)

			inline void ch_fire_closed(int code) const {
				NETP_ASSERT(L->in_event_loop());

				NETP_ASSERT(m_pipeline != nullptr);
				m_pipeline->fire_closed();

				NETP_ASSERT(m_ch_close_p != nullptr);
				m_ch_close_p->set(code);
			}

			inline void ch_deinit() {
				NETP_ASSERT(m_pipeline != nullptr);
				m_pipeline->deinit();
				m_pipeline = nullptr;
			}

			inline void ch_init() {
				NETP_ASSERT((m_chflag&int(channel_flag::F_CLOSED)));
				m_chflag &= ~int(channel_flag::F_CLOSED);

				NETP_ASSERT(m_ch_close_p == nullptr);
				m_ch_close_p = netp::make_ref<promise<int>>();
				m_ch_close_p->if_done([ch=NRP<channel>(this)](int const&) {
					ch->ch_deinit();
				});

				m_pipeline = netp::make_ref<channel_pipeline>(NRP<channel>(this));
				m_pipeline->init();
			}

			inline void ch_rdwr_shutdown_check() {
				NETP_ASSERT(L->in_event_loop());
				if ( ((m_chflag&(int(channel_flag::F_CLOSED)|int(channel_flag::F_CLOSING)))==0) && ((m_chflag & (int(channel_flag::F_READWRITE_SHUTDOWN))) == int(channel_flag::F_READWRITE_SHUTDOWN)) ) {
					NETP_TRACE_CHANNEL("[channel][%s]ch_rdwr_shutdown_check trigger ch_close_impl()", ch_info().c_str() );
					ch_close_impl(nullptr);
				}
			}

	public:
		channel(NRP<io_event_loop> const& L_) :
			L(L_),
			m_chflag(int(channel_flag::F_CLOSED)),
			m_cherrno(0),
			m_pipeline(nullptr),
			m_ch_close_p(nullptr),
			m_ctx(nullptr)
		{
			NETP_TRACE_CHANNEL("channel::channel()");
		}

		~channel() {
			NETP_TRACE_CHANNEL("channel::~channel()");
		}

		//__NETP_FORCE_INLINE NRP<io_event_loop> const& event_loop() const { return m_loop;}
		__NETP_FORCE_INLINE NRP<channel_pipeline> const& pipeline() const { return m_pipeline;}
		__NETP_FORCE_INLINE NRP<promise<int>> const& ch_close_promise() const { return m_ch_close_p;}

		template <class ctx_t>
		inline NRP<ctx_t> get_ctx() const {
			return netp::static_pointer_cast<ctx_t>(m_ctx);
		}
		inline void set_ctx(NRP<ref_base> const& ctx) {
			m_ctx = ctx;
		}

		__NETP_FORCE_INLINE int& ch_errno() { return m_cherrno; }
		__NETP_FORCE_INLINE int& ch_flag() { return m_chflag; }

		inline bool ch_is_active() { return (m_chflag & int(channel_flag::F_ACTIVE)) != 0 ; }
		inline bool ch_is_passive() { return !ch_is_active(); }
		inline bool ch_is_listener() { return (m_chflag & int(channel_flag::F_LISTENING)) != 0; }

		inline void ch_set_active() { m_chflag |= int(channel_flag::F_ACTIVE); }
		inline void ch_set_connected() {
			m_chflag &= ~(int(channel_flag::F_CLOSED)|int(channel_flag::F_CONNECTING));
			m_chflag |= int(channel_flag::F_CONNECTED);
		}
		
#define CH_FUTURE_ACTION_IMPL_CH_PROMISE_1(NAME) \
private: \
		inline void __ch_##NAME(NRP<promise<int>> const& chp) {\
			if (m_pipeline == nullptr) { \
					chp->set(netp::E_CHANNEL_CLOSED); \
				return; \
			} \
			m_pipeline->NAME(chp); \
		} \
public: \
		inline NRP<promise<int>> ch_##NAME() {\
			const NRP<promise<int>> chf = netp::make_ref<promise<int>>(); \
			channel::ch_##NAME(chf); \
			return chf;\
		} \
		inline void ch_##NAME(NRP<promise<int>> const& chp) { \
			L->execute([_ch=NRP<channel>(this), chp]() { \
				_ch->__ch_##NAME(chp); \
			}); \
		} \

		CH_FUTURE_ACTION_IMPL_CH_PROMISE_1(close)
		CH_FUTURE_ACTION_IMPL_CH_PROMISE_1(close_read)
		CH_FUTURE_ACTION_IMPL_CH_PROMISE_1(close_write)

#define CH_FUTURE_ACTION_IMPL_PACKET(NAME) \
private: \
		inline void __ch_##NAME(NRP<packet> const& outlet, NRP<promise<int>> const& chp) {\
			if (m_pipeline == nullptr) { \
					chp->set(netp::E_CHANNEL_CLOSED); \
				return; \
			} \
			m_pipeline->NAME(outlet,chp); \
		} \
public: \
		inline NRP<promise<int>> ch_##NAME(NRP<packet> const& outlet) {\
			const NRP<promise<int>> chf = netp::make_ref<promise<int>>(); \
			ch_##NAME(outlet,chf); \
			return chf; \
		} \
		inline void ch_##NAME(NRP<packet> const& outlet, NRP<promise<int>> const& chp) {\
			L->execute([_ch=NRP<channel>(this), outlet, chp]() { \
				_ch->__ch_##NAME(outlet, chp); \
			}); \
		} \

		CH_FUTURE_ACTION_IMPL_PACKET(write);

#define CH_FUTURE_ACTION_IMPL_PACKET_ADDR(NAME) \
private: \
		inline void __ch_##NAME(NRP<packet> const& outlet, address const& to, NRP<promise<int>> const& chp) {\
			if (m_pipeline == nullptr) { \
					chp->set(netp::E_CHANNEL_CLOSED); \
				return; \
			} \
			m_pipeline->NAME(outlet,to,chp); \
		} \
public: \
		inline NRP<promise<int>> ch_##NAME(NRP<packet> const& outlet, address const& to) {\
			const NRP<promise<int>> chf = netp::make_ref<promise<int>>(); \
			ch_##NAME(outlet,to,chf); \
			return chf; \
		} \
		inline void ch_##NAME(NRP<packet> const& outlet, address const& to, NRP<promise<int>> const& chp) {\
			L->execute([_ch=NRP<channel>(this), outlet, to,chp]() { \
				_ch->__ch_##NAME(outlet,to, chp); \
			}); \
		} \

	CH_FUTURE_ACTION_IMPL_PACKET_ADDR(write_to);


#define CH_ACTION_IMPL_VOID(NAME) \
private: \
		inline void __ch_##NAME() { \
			if (m_pipeline == nullptr) { \
				return; \
			} \
			m_pipeline->NAME(); \
		} \
public: \
		inline void ch_##NAME() {\
			L->execute([_ch=NRP<channel>(this)]() { \
				_ch->__ch_##NAME(); \
			}); \
		} \

		virtual channel_id_t ch_id() const = 0; //called by context in event_loop
		virtual std::string ch_info() const = 0;

		virtual NRP<promise<int>> ch_set_read_buffer_size(u32_t size) = 0;
		virtual NRP<promise<int>> ch_get_read_buffer_size() = 0;

		virtual NRP<promise<int>> ch_set_write_buffer_size(u32_t size) = 0;
		virtual NRP<promise<int>> ch_get_write_buffer_size() = 0;
		virtual NRP<promise<int>> ch_set_nodelay() = 0;
		virtual void ch_set_bdlimit(u32_t) {};

		virtual void ch_write_impl(NRP<packet> const& outlet, NRP<promise<int>> const& chp) = 0;
		virtual void ch_write_to_impl(NRP<packet> const& outlet, netp::address const& to, NRP<promise<int>> const& chp) { 
			NETP_ASSERT("to_impl"); 
		
			(void)outlet;
			(void)to;
			(void)chp;
		};

		virtual void ch_close_read_impl(NRP<promise<int>> const& chp) = 0;
		virtual void ch_close_write_impl(NRP<promise<int>> const& chp) = 0;
		virtual void ch_close_impl(NRP<promise<int>> const& chp) = 0;


		virtual void ch_aio_read( fn_aio_event_t const& fn = nullptr) = 0;
		virtual void ch_aio_end_read() = 0;

		virtual void ch_aio_write(fn_aio_event_t const& fn = nullptr) = 0;
		virtual void ch_aio_end_write() = 0;

		virtual void ch_aio_connect( fn_aio_event_t const& fn ) = 0;
		virtual void ch_aio_end_connect() = 0;
	};
}
#endif