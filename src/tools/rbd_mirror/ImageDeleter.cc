// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2016 SUSE LINUX GmbH
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 *
 */

#include <boost/bind.hpp>
#include <map>
#include <set>
#include <sstream>

#include "include/rados/librados.hpp"
#include "common/Formatter.h"
#include "common/admin_socket.h"
#include "common/debug.h"
#include "common/errno.h"
#include "common/WorkQueue.h"
#include "global/global_context.h"
#include "librbd/internal.h"
#include "librbd/ImageCtx.h"
#include "librbd/ImageState.h"
#include "librbd/Journal.h"
#include "librbd/Operations.h"
#include "librbd/image/RemoveRequest.h"
#include "cls/rbd/cls_rbd_client.h"
#include "cls/rbd/cls_rbd_types.h"
#include "librbd/Utils.h"
#include "ImageDeleter.h"
#include "tools/rbd_mirror/image_deleter/RemoveRequest.h"

#define dout_context g_ceph_context
#define dout_subsys ceph_subsys_rbd_mirror
#undef dout_prefix
#define dout_prefix *_dout << "rbd::mirror::ImageDeleter: " << this << " " \
                           << __func__ << ": "

using std::string;
using std::map;
using std::stringstream;
using std::vector;
using std::pair;
using std::make_pair;

using librados::IoCtx;
using namespace librbd;

namespace rbd {
namespace mirror {

namespace {

class ImageDeleterAdminSocketCommand {
public:
  virtual ~ImageDeleterAdminSocketCommand() {}
  virtual bool call(Formatter *f, stringstream *ss) = 0;
};

template <typename I>
class StatusCommand : public ImageDeleterAdminSocketCommand {
public:
  explicit StatusCommand(ImageDeleter<I> *image_del) : image_del(image_del) {}

  bool call(Formatter *f, stringstream *ss) override {
    image_del->print_status(f, ss);
    return true;
  }

private:
  ImageDeleter<I> *image_del;
};

} // anonymous namespace

template <typename I>
class ImageDeleterAdminSocketHook : public AdminSocketHook {
public:
  ImageDeleterAdminSocketHook(CephContext *cct, ImageDeleter<I> *image_del) :
    admin_socket(cct->get_admin_socket()) {

    std::string command;
    int r;

    command = "rbd mirror deletion status";
    r = admin_socket->register_command(command, command, this,
				       "get status for image deleter");
    if (r == 0) {
      commands[command] = new StatusCommand<I>(image_del);
    }

  }

  ~ImageDeleterAdminSocketHook() override {
    for (Commands::const_iterator i = commands.begin(); i != commands.end();
	 ++i) {
      (void)admin_socket->unregister_command(i->first);
      delete i->second;
    }
  }

  bool call(std::string command, cmdmap_t& cmdmap, std::string format,
	    bufferlist& out) override {
    Commands::const_iterator i = commands.find(command);
    assert(i != commands.end());
    Formatter *f = Formatter::create(format);
    stringstream ss;
    bool r = i->second->call(f, &ss);
    delete f;
    out.append(ss);
    return r;
  }

private:
  typedef std::map<std::string, ImageDeleterAdminSocketCommand*> Commands;
  AdminSocket *admin_socket;
  Commands commands;
};

template <typename I>
ImageDeleter<I>::ImageDeleter(ContextWQ *work_queue, SafeTimer *timer,
                              Mutex *timer_lock,
                              ServiceDaemon<librbd::ImageCtx>* service_daemon)
  : m_work_queue(work_queue),
    m_service_daemon(service_daemon),
    m_delete_lock("rbd::mirror::ImageDeleter::Delete"),
    m_image_deleter_thread(this),
    m_failed_timer(timer),
    m_failed_timer_lock(timer_lock),
    m_asok_hook(new ImageDeleterAdminSocketHook<I>(g_ceph_context, this))
{
  set_failed_timer_interval(g_ceph_context->_conf->get_val<double>(
    "rbd_mirror_delete_retry_interval"));
  m_image_deleter_thread.create("image_deleter");
}

template <typename I>
ImageDeleter<I>::~ImageDeleter() {
  dout(20) << "enter" << dendl;

  m_running = false;
  {
    Mutex::Locker l (m_delete_lock);
    m_delete_queue_cond.Signal();
  }
  if (m_image_deleter_thread.is_started()) {
    m_image_deleter_thread.join();
  }

  delete m_asok_hook;
  dout(20) << "return" << dendl;
}

template <typename I>
void ImageDeleter<I>::run() {
  dout(20) << "enter" << dendl;
  while(m_running) {
    m_delete_lock.Lock();
    while (m_delete_queue.empty()) {
      dout(20) << "waiting for delete requests" << dendl;
      m_delete_queue_cond.Wait(m_delete_lock);

      if (!m_running) {
        m_delete_lock.Unlock();
        dout(20) << "return" << dendl;
        return;
      }
    }

    m_active_delete = std::move(m_delete_queue.back());
    m_delete_queue.pop_back();
    m_delete_lock.Unlock();

    bool move_to_next = process_image_delete();
    if (!move_to_next) {
      if (!m_running) {
       dout(20) << "return" << dendl;
       return;
      }

      Mutex::Locker l(m_delete_lock);
      if (m_delete_queue.size() == 1) {
        m_delete_queue_cond.Wait(m_delete_lock);
      }
    }
  }
}

template <typename I>
void ImageDeleter<I>::schedule_image_delete(IoCtxRef local_io_ctx,
                                            const std::string& global_image_id,
                                            bool ignore_orphaned) {
  dout(20) << "enter" << dendl;

  Mutex::Locker locker(m_delete_lock);
  int64_t local_pool_id = local_io_ctx->get_id();
  auto del_info = find_delete_info(local_pool_id, global_image_id);
  if (del_info != nullptr) {
    dout(20) << "image " << global_image_id << " "
             << "was already scheduled for deletion" << dendl;
    if (ignore_orphaned) {
      (*del_info)->ignore_orphaned = true;
    }
    return;
  }

  m_delete_queue.push_front(
    unique_ptr<DeleteInfo>(new DeleteInfo(local_pool_id, global_image_id,
                                          local_io_ctx, ignore_orphaned)));
  m_delete_queue_cond.Signal();
}

template <typename I>
void ImageDeleter<I>::wait_for_scheduled_deletion(int64_t local_pool_id,
                                                  const std::string &global_image_id,
                                                  Context *ctx,
                                                  bool notify_on_failed_retry) {

  ctx = new FunctionContext([this, ctx](int r) {
      m_work_queue->queue(ctx, r);
    });

  Mutex::Locker locker(m_delete_lock);
  auto del_info = find_delete_info(local_pool_id, global_image_id);
  if (!del_info) {
    // image not scheduled for deletion
    ctx->complete(0);
    return;
  }

  dout(20) << "local_pool_id=" << local_pool_id << ", "
           << "global_image_id=" << global_image_id << dendl;

  if ((*del_info)->on_delete != nullptr) {
    (*del_info)->on_delete->complete(-ESTALE);
  }
  (*del_info)->on_delete = ctx;
  (*del_info)->notify_on_failed_retry = notify_on_failed_retry;
}

template <typename I>
void ImageDeleter<I>::cancel_waiter(int64_t local_pool_id,
                                    const std::string &global_image_id) {
  Mutex::Locker locker(m_delete_lock);
  auto del_info = find_delete_info(local_pool_id, global_image_id);
  if (!del_info) {
    return;
  }

  if ((*del_info)->on_delete != nullptr) {
    (*del_info)->on_delete->complete(-ECANCELED);
    (*del_info)->on_delete = nullptr;
  }
}

template <typename I>
bool ImageDeleter<I>::process_image_delete() {
  stringstream ss;
  m_active_delete->to_string(ss);
  std::string del_info_str = ss.str();
  dout(10) << "start processing delete request: " << del_info_str << dendl;

  C_SaferCond remove_ctx;
  image_deleter::ErrorResult error_result;
  auto req = image_deleter::RemoveRequest<I>::create(
    *m_active_delete->local_io_ctx, m_active_delete->global_image_id,
    m_active_delete->ignore_orphaned, &error_result, m_work_queue, &remove_ctx);
  req->send();

  int r = remove_ctx.wait();
  if (r < 0) {
    if (error_result == image_deleter::ERROR_RESULT_COMPLETE) {
      complete_active_delete(r);
      return true;
    } else if (error_result == image_deleter::ERROR_RESULT_RETRY_IMMEDIATELY) {
      Mutex::Locker l(m_delete_lock);
      m_active_delete->notify(r);
      m_delete_queue.push_front(std::move(m_active_delete));
      return false;
    } else {
      enqueue_failed_delete(r);
      return true;
    }
  }

  complete_active_delete(0);
  return true;
}

template <typename I>
void ImageDeleter<I>::complete_active_delete(int r) {
  dout(20) << dendl;

  Mutex::Locker delete_locker(m_delete_lock);
  m_active_delete->notify(r);
  m_active_delete.reset();
}

template <typename I>
void ImageDeleter<I>::enqueue_failed_delete(int error_code) {
  dout(20) << "enter" << dendl;

  if (error_code == -EBLACKLISTED) {
    derr << "blacklisted while deleting local image" << dendl;
    complete_active_delete(error_code);
    return;
  }

  m_delete_lock.Lock();
  if (m_active_delete->notify_on_failed_retry) {
    m_active_delete->notify(error_code);
  }
  m_active_delete->error_code = error_code;
  bool was_empty = m_failed_queue.empty();
  m_failed_queue.push_front(std::move(m_active_delete));
  m_delete_lock.Unlock();
  if (was_empty) {
    FunctionContext *ctx = new FunctionContext(
      boost::bind(&ImageDeleter<I>::retry_failed_deletions, this));
    Mutex::Locker l(*m_failed_timer_lock);
    m_failed_timer->add_event_after(m_failed_interval, ctx);
  }
}

template <typename I>
void ImageDeleter<I>::retry_failed_deletions() {
  dout(20) << "enter" << dendl;

  Mutex::Locker l(m_delete_lock);

  bool empty = m_failed_queue.empty();
  while (!m_failed_queue.empty()) {
    m_delete_queue.push_back(std::move(m_failed_queue.back()));
    m_delete_queue.back()->retries++;
    m_failed_queue.pop_back();
  }
  if (!empty) {
    m_delete_queue_cond.Signal();
  }
}

template <typename I>
unique_ptr<typename ImageDeleter<I>::DeleteInfo> const*
ImageDeleter<I>::find_delete_info(int64_t local_pool_id,
                                  const std::string &global_image_id) {
  assert(m_delete_lock.is_locked());

  if (m_active_delete && m_active_delete->match(local_pool_id,
                                                global_image_id)) {
    return &m_active_delete;
  }

  for (const auto& del_info : m_delete_queue) {
    if (del_info->match(local_pool_id, global_image_id)) {
      return &del_info;
    }
  }

  for (const auto& del_info : m_failed_queue) {
    if (del_info->match(local_pool_id, global_image_id)) {
      return &del_info;
    }
  }

  return nullptr;
}

template <typename I>
void ImageDeleter<I>::print_status(Formatter *f, stringstream *ss) {
  dout(20) << "enter" << dendl;

  if (f) {
    f->open_object_section("image_deleter_status");
    f->open_array_section("delete_images_queue");
  }

  Mutex::Locker l(m_delete_lock);
  for (const auto& image : m_delete_queue) {
    image->print_status(f, ss);
  }

  if (f) {
    f->close_section();
    f->open_array_section("failed_deletes_queue");
  }

  for (const auto& image : m_failed_queue) {
    image->print_status(f, ss, true);
  }

  if (f) {
    f->close_section();
    f->close_section();
    f->flush(*ss);
  }
}

template <typename I>
void ImageDeleter<I>::DeleteInfo::notify(int r) {
  if (on_delete) {
    dout(20) << "executing image deletion handler r=" << r << dendl;

    Context *ctx = on_delete;
    on_delete = nullptr;
    ctx->complete(r);
  }
}

template <typename I>
void ImageDeleter<I>::DeleteInfo::to_string(stringstream& ss) {
  ss << "[" << "local_pool_id=" << local_pool_id << ", ";
  ss << "global_image_id=" << global_image_id << "]";
}

template <typename I>
void ImageDeleter<I>::DeleteInfo::print_status(Formatter *f, stringstream *ss,
                                               bool print_failure_info) {
  if (f) {
    f->open_object_section("delete_info");
    f->dump_int("local_pool_id", local_pool_id);
    f->dump_string("global_image_id", global_image_id);
    if (print_failure_info) {
      f->dump_string("error_code", cpp_strerror(error_code));
      f->dump_int("retries", retries);
    }
    f->close_section();
    f->flush(*ss);
  } else {
    this->to_string(*ss);
  }
}

template <typename I>
vector<string> ImageDeleter<I>::get_delete_queue_items() {
  vector<string> items;

  Mutex::Locker l(m_delete_lock);
  for (const auto& del_info : m_delete_queue) {
    items.push_back(del_info->global_image_id);
  }

  return items;
}

template <typename I>
vector<pair<string, int> > ImageDeleter<I>::get_failed_queue_items() {
  vector<pair<string, int> > items;

  Mutex::Locker l(m_delete_lock);
  for (const auto& del_info : m_failed_queue) {
    items.push_back(make_pair(del_info->global_image_id,
                              del_info->error_code));
  }

  return items;
}

template <typename I>
void ImageDeleter<I>::set_failed_timer_interval(double interval) {
  this->m_failed_interval = interval;
}

} // namespace mirror
} // namespace rbd

template class rbd::mirror::ImageDeleter<librbd::ImageCtx>;
