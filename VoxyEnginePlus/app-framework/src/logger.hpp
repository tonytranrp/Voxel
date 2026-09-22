#pragma once

#include <fstream>
#include <iostream>
#include <memory>
#include <streambuf>
#include <string>

/// Mirrors everything written to std::cout / std::cerr into a log file next to the executable.
///
/// Without this, a packaged build that fails at startup just closes its window and the user has
/// nothing to report. With it there is always a voxy-log.txt to read or send.
class Logger
{
  public:
    static void Init(std::string const &path = "voxy-log.txt")
    {
        auto &self = Instance();
        self.file_.open(path, std::ios::out | std::ios::trunc);
        if (!self.file_.is_open())
            return; // read-only directory: keep going, just without a log

        self.out_tee_ = std::make_unique<TeeBuf>(std::cout.rdbuf(), self.file_.rdbuf());
        self.err_tee_ = std::make_unique<TeeBuf>(std::cerr.rdbuf(), self.file_.rdbuf());
        self.old_out_ = std::cout.rdbuf(self.out_tee_.get());
        self.old_err_ = std::cerr.rdbuf(self.err_tee_.get());
    }

    static void Shutdown()
    {
        auto &self = Instance();
        if (self.old_out_ != nullptr)
            std::cout.rdbuf(self.old_out_);
        if (self.old_err_ != nullptr)
            std::cerr.rdbuf(self.old_err_);
        self.old_out_ = nullptr;
        self.old_err_ = nullptr;
        self.out_tee_.reset();
        self.err_tee_.reset();
        if (self.file_.is_open())
            self.file_.close();
    }

  private:
    class TeeBuf : public std::streambuf
    {
      public:
        TeeBuf(std::streambuf *a, std::streambuf *b)
            : a_(a), b_(b)
        {
        }

      protected:
        int overflow(int c) override
        {
            if (c == EOF)
                return !EOF;
            int const r1 = a_ != nullptr ? a_->sputc(static_cast<char>(c)) : c;
            int const r2 = b_ != nullptr ? b_->sputc(static_cast<char>(c)) : c;
            return (r1 == EOF || r2 == EOF) ? EOF : c;
        }

        int sync() override
        {
            int const r1 = a_ != nullptr ? a_->pubsync() : 0;
            int const r2 = b_ != nullptr ? b_->pubsync() : 0;
            return (r1 == 0 && r2 == 0) ? 0 : -1;
        }

      private:
        std::streambuf *a_;
        std::streambuf *b_;
    };

    static Logger &Instance()
    {
        static Logger instance;
        return instance;
    }

    std::ofstream file_;
    std::unique_ptr<TeeBuf> out_tee_;
    std::unique_ptr<TeeBuf> err_tee_;
    std::streambuf *old_out_ = nullptr;
    std::streambuf *old_err_ = nullptr;
};
